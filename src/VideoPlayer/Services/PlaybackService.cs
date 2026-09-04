using System.Diagnostics;
using System.IO;
using LibVLCSharp.Shared;
using VideoPlayer.Helpers;

namespace VideoPlayer.Services;

public sealed class PlaybackService : IDisposable
{
    private readonly object _lifecycleGate = new();
    private readonly object _stateGate = new();
    private readonly LibVLC _libVlc;
    private readonly MediaPlayer _mediaPlayer;
    private Media? _currentMedia;
    private int _stateValue = (int)PlaybackState.Idle;
    private long _stateGeneration;
    private long _generation;
    private int _hasMediaValue;
    private int _seekableValue;
    private long _length;
    private int _volume = 100;
    private int _previousNonZeroVolume = 100;
    private long _videoTrackValidationGeneration = -1;
    private long _videoElementaryStreamGeneration = -1;
    private volatile bool _isReleasingMedia;
    private volatile bool _disposed;

#if DEBUG
    private readonly Stopwatch _openingStopwatch = new();
#endif

    public PlaybackService()
    {
        _libVlc = new LibVLC(enableDebugLogs: false);
        try
        {
            _mediaPlayer = new MediaPlayer(_libVlc)
            {
                Volume = _volume,
            };
        }
        catch
        {
            _libVlc.Dispose();
            throw;
        }

        SubscribeToPlayerEvents();
    }

    public event EventHandler<PlaybackStateChangedEventArgs>? StateChanged;

    public event EventHandler<PlaybackLengthChangedEventArgs>? LengthChanged;

    public event EventHandler<PlaybackSeekableChangedEventArgs>? SeekableChanged;

    public event EventHandler<PlaybackVolumeChangedEventArgs>? VolumeChanged;

    public event EventHandler<PlaybackFailedEventArgs>? PlaybackFailed;

    public MediaPlayer MediaPlayer => _mediaPlayer;

    public PlaybackState State => (PlaybackState)Volatile.Read(ref _stateValue);

    public long Generation => Volatile.Read(ref _generation);

    public bool HasMedia => Volatile.Read(ref _hasMediaValue) != 0;

    public bool IsSeekable => Volatile.Read(ref _seekableValue) != 0;

    public long Length => Math.Max(0, Volatile.Read(ref _length));

    public int Volume => Volatile.Read(ref _volume);

    public bool IsMuted => Volume == 0;

    public long CurrentTime
    {
        get
        {
            if (_disposed || !HasMedia || State is PlaybackState.Stopped or PlaybackState.Ended)
            {
                return 0;
            }

            try
            {
                return PlaybackMath.ClampPosition(_mediaPlayer.Time, Length);
            }
            catch (ObjectDisposedException)
            {
                return 0;
            }
        }
    }

    public void OpenAndPlay(string filePath)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (string.IsNullOrWhiteSpace(filePath))
        {
            throw new ArgumentException("Шлях до файла не вказано.", nameof(filePath));
        }

        var fullPath = Path.GetFullPath(filePath);
        if (!File.Exists(fullPath))
        {
            throw new FileNotFoundException("Відеофайл не знайдено.", fullPath);
        }

        lock (_lifecycleGate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            Media? stagedMedia = null;
            var ownsNewMedia = false;
            try
            {
                // Constructing first preserves the currently playing file when the new path
                // cannot even be represented by LibVLC.
                stagedMedia = new Media(_libVlc, fullPath, FromType.FromPath);

                _isReleasingMedia = true;
                try
                {
                    ReleaseCurrentMediaFromPlayer();
                }
                finally
                {
                    _isReleasingMedia = false;
                }

                var generation = Interlocked.Increment(ref _generation);
                _currentMedia = stagedMedia;
                stagedMedia = null;
                ownsNewMedia = true;
                Volatile.Write(ref _hasMediaValue, 1);
                Volatile.Write(ref _seekableValue, 0);
                Volatile.Write(ref _length, 0);
                _mediaPlayer.Volume = Volume;
                PublishState(PlaybackState.Opening, generation);

#if DEBUG
                _openingStopwatch.Restart();
#endif

                if (!_mediaPlayer.Play(_currentMedia))
                {
                    throw new InvalidOperationException("LibVLC не зміг розпочати відтворення файла.");
                }
            }
            catch
            {
                if (stagedMedia is not null)
                {
                    TryReleaseNativeResource(stagedMedia.Dispose, "звільнення підготовленого Media");
                }

                if (ownsNewMedia && _currentMedia is not null)
                {
                    RollBackFailedOpen();
                    PublishState(PlaybackState.Error);
                }

                throw;
            }
            finally
            {
                _isReleasingMedia = false;
            }
        }
    }

    public void TogglePlayPause()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!HasMedia)
        {
            return;
        }

        if (State == PlaybackState.Playing || _mediaPlayer.IsPlaying)
        {
            _mediaPlayer.SetPause(true);
            return;
        }

        if (State == PlaybackState.Ended)
        {
            _mediaPlayer.Stop();
            _mediaPlayer.Time = 0;
        }

        if (!_mediaPlayer.Play())
        {
            PublishState(PlaybackState.Error);
            SafeRaise(
                PlaybackFailed,
                new PlaybackFailedEventArgs(Generation, PlaybackFailureReason.Decoding));
        }
    }

    public void Stop()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!HasMedia)
        {
            return;
        }

        _mediaPlayer.Stop();
        PublishState(PlaybackState.Stopped);
    }

    public bool SeekTo(long positionMilliseconds)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!HasMedia
            || !IsSeekable
            || Length <= 0
            || State is not (PlaybackState.Playing or PlaybackState.Paused))
        {
            return false;
        }

        _mediaPlayer.Time = PlaybackMath.ClampPosition(positionMilliseconds, Length);
        return true;
    }

    public bool SeekBy(int offsetSeconds)
    {
        return SeekTo(PlaybackMath.CalculateSeekTarget(CurrentTime, offsetSeconds, Length));
    }

    public void SetVolume(int volume)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        var clampedVolume = PlaybackMath.ClampVolume(volume);
        if (clampedVolume > 0)
        {
            Volatile.Write(ref _previousNonZeroVolume, clampedVolume);
        }

        _mediaPlayer.Volume = clampedVolume;
        var previousVolume = Interlocked.Exchange(ref _volume, clampedVolume);
        if (previousVolume != clampedVolume)
        {
            SafeRaise(VolumeChanged, new PlaybackVolumeChangedEventArgs(clampedVolume));
        }
    }

    public void ToggleMute()
    {
        SetVolume(IsMuted ? Math.Max(1, Volatile.Read(ref _previousNonZeroVolume)) : 0);
    }

    public void Dispose()
    {
        lock (_lifecycleGate)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            UnsubscribeFromPlayerEvents();

            TryReleaseNativeResource(_mediaPlayer.Stop, "зупинки MediaPlayer");
            TryReleaseNativeResource(() => _mediaPlayer.Media = null, "від'єднання Media");
            TryReleaseNativeResource(() => _currentMedia?.Dispose(), "звільнення Media");
            _currentMedia = null;
            Volatile.Write(ref _hasMediaValue, 0);
            TryReleaseNativeResource(_mediaPlayer.Dispose, "звільнення MediaPlayer");
            TryReleaseNativeResource(_libVlc.Dispose, "звільнення LibVLC");
        }
    }

    private void SubscribeToPlayerEvents()
    {
        _mediaPlayer.Opening += OnOpening;
        _mediaPlayer.Playing += OnPlaying;
        _mediaPlayer.Paused += OnPaused;
        _mediaPlayer.Stopped += OnStopped;
        _mediaPlayer.EndReached += OnEndReached;
        _mediaPlayer.EncounteredError += OnEncounteredError;
        _mediaPlayer.LengthChanged += OnLengthChanged;
        _mediaPlayer.SeekableChanged += OnSeekableChanged;
        _mediaPlayer.ESAdded += OnElementaryStreamAdded;
    }

    private void UnsubscribeFromPlayerEvents()
    {
        _mediaPlayer.Opening -= OnOpening;
        _mediaPlayer.Playing -= OnPlaying;
        _mediaPlayer.Paused -= OnPaused;
        _mediaPlayer.Stopped -= OnStopped;
        _mediaPlayer.EndReached -= OnEndReached;
        _mediaPlayer.EncounteredError -= OnEncounteredError;
        _mediaPlayer.LengthChanged -= OnLengthChanged;
        _mediaPlayer.SeekableChanged -= OnSeekableChanged;
        _mediaPlayer.ESAdded -= OnElementaryStreamAdded;
    }

    private void ReleaseCurrentMediaFromPlayer()
    {
        if (_currentMedia is null)
        {
            return;
        }

        _mediaPlayer.Stop();
        _mediaPlayer.Media = null;
        _currentMedia.Dispose();
        _currentMedia = null;
        Volatile.Write(ref _hasMediaValue, 0);
        Volatile.Write(ref _seekableValue, 0);
        Volatile.Write(ref _length, 0);
    }

    private void RollBackFailedOpen()
    {
        _isReleasingMedia = true;
        try
        {
            TryReleaseNativeResource(_mediaPlayer.Stop, "відкату MediaPlayer");
            TryReleaseNativeResource(() => _mediaPlayer.Media = null, "від'єднання невдалого Media");
            TryReleaseNativeResource(() => _currentMedia?.Dispose(), "звільнення невдалого Media");
            _currentMedia = null;
            Volatile.Write(ref _hasMediaValue, 0);
            Volatile.Write(ref _seekableValue, 0);
            Volatile.Write(ref _length, 0);
        }
        finally
        {
            _isReleasingMedia = false;
        }
    }

    private void OnOpening(object? sender, EventArgs e)
    {
        if (!ShouldIgnorePlayerCallback())
        {
            PublishState(PlaybackState.Opening);
        }
    }

    private void OnPlaying(object? sender, EventArgs e)
    {
        if (ShouldIgnorePlayerCallback())
        {
            return;
        }

        var generation = Generation;

#if DEBUG
        if (_openingStopwatch.IsRunning)
        {
            _openingStopwatch.Stop();
            Debug.WriteLine($"VideoPlayer: файл почав відтворюватися через {_openingStopwatch.ElapsedMilliseconds} мс.");
        }
#endif

        PublishState(PlaybackState.Playing, generation);
        ScheduleVideoTrackValidation(generation);
    }

    private void OnPaused(object? sender, EventArgs e)
    {
        if (!ShouldIgnorePlayerCallback())
        {
            PublishState(PlaybackState.Paused);
        }
    }

    private void OnStopped(object? sender, EventArgs e)
    {
        if (!ShouldIgnorePlayerCallback())
        {
            PublishState(PlaybackState.Stopped);
        }
    }

    private void OnEndReached(object? sender, EventArgs e)
    {
        if (!ShouldIgnorePlayerCallback())
        {
            PublishState(PlaybackState.Ended);
        }
    }

    private void OnEncounteredError(object? sender, EventArgs e)
    {
        if (ShouldIgnorePlayerCallback())
        {
            return;
        }

        PublishState(PlaybackState.Error);
        SafeRaise(
            PlaybackFailed,
            new PlaybackFailedEventArgs(Generation, PlaybackFailureReason.Decoding));
    }

    private void OnLengthChanged(object? sender, MediaPlayerLengthChangedEventArgs e)
    {
        if (ShouldIgnorePlayerCallback())
        {
            return;
        }

        var length = Math.Max(0, e.Length);
        var generation = Generation;
        Volatile.Write(ref _length, length);
        SafeRaise(LengthChanged, new PlaybackLengthChangedEventArgs(length, generation));
    }

    private void OnSeekableChanged(object? sender, MediaPlayerSeekableChangedEventArgs e)
    {
        if (ShouldIgnorePlayerCallback())
        {
            return;
        }

        var isSeekable = e.Seekable != 0;
        var generation = Generation;
        Volatile.Write(ref _seekableValue, isSeekable ? 1 : 0);
        SafeRaise(
            SeekableChanged,
            new PlaybackSeekableChangedEventArgs(isSeekable, generation));
    }

    private void OnElementaryStreamAdded(object? sender, MediaPlayerESAddedEventArgs e)
    {
        if (!ShouldIgnorePlayerCallback() && e.Type == TrackType.Video)
        {
            Volatile.Write(ref _videoElementaryStreamGeneration, Generation);
        }
    }

    private void ScheduleVideoTrackValidation(long generation)
    {
        if (Interlocked.Exchange(ref _videoTrackValidationGeneration, generation) != generation)
        {
            _ = ValidateVideoTrackAsync(generation);
        }
    }

    private async Task ValidateVideoTrackAsync(long generation)
    {
        var validationCompleted = false;

        try
        {
            var consecutiveAudioOnlySnapshots = 0;
            for (var attempt = 0; attempt < 6; attempt++)
            {
                await Task.Delay(
                        attempt == 0
                            ? TimeSpan.FromSeconds(1)
                            : TimeSpan.FromSeconds(1.5))
                    .ConfigureAwait(false);

                if (_disposed
                    || generation != Generation
                    || State is PlaybackState.Stopped or PlaybackState.Error)
                {
                    return;
                }

                if (Volatile.Read(ref _videoElementaryStreamGeneration) == generation)
                {
                    validationCompleted = true;
                    return;
                }

                MediaTrack[] tracks;
                lock (_lifecycleGate)
                {
                    if (_disposed || generation != Generation || _currentMedia is null)
                    {
                        return;
                    }

                    tracks = _currentMedia.Tracks;
                }

                if (tracks.Any(track => track.TrackType == TrackType.Video))
                {
                    validationCompleted = true;
                    return;
                }

                consecutiveAudioOnlySnapshots = tracks.Any(
                    track => track.TrackType == TrackType.Audio)
                    ? consecutiveAudioOnlySnapshots + 1
                    : 0;

                var playbackState = State;
                if (consecutiveAudioOnlySnapshots >= 3 || playbackState == PlaybackState.Ended)
                {
                    if (!tracks.Any(track => track.TrackType == TrackType.Audio)
                        || _disposed
                        || generation != Generation
                        || Volatile.Read(ref _videoElementaryStreamGeneration) == generation
                        || playbackState is not (PlaybackState.Playing
                            or PlaybackState.Paused
                            or PlaybackState.Ended))
                    {
                        continue;
                    }

                    validationCompleted = TryPublishNoVideoFailure(generation);
                    return;
                }
            }
        }
        catch (Exception exception) when (exception is ObjectDisposedException or VLCException)
        {
            Debug.WriteLine($"VideoPlayer: перевірку відеодоріжки скасовано: {exception.Message}");
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: перевірка відеодоріжки завершилася помилкою: {exception}");
        }
        finally
        {
            if (!validationCompleted)
            {
                Interlocked.CompareExchange(
                    ref _videoTrackValidationGeneration,
                    -1,
                    generation);
            }
        }
    }

    private bool ShouldIgnorePlayerCallback() => _disposed || _isReleasingMedia;

    private bool TryPublishNoVideoFailure(long generation)
    {
        lock (_stateGate)
        {
            var state = (PlaybackState)_stateValue;
            if (_disposed
                || generation != Generation
                || _stateGeneration != generation
                || state is not (PlaybackState.Playing
                    or PlaybackState.Paused
                    or PlaybackState.Ended))
            {
                return false;
            }

            Volatile.Write(ref _stateValue, (int)PlaybackState.Error);
        }

        SafeRaise(
            StateChanged,
            new PlaybackStateChangedEventArgs(PlaybackState.Error, generation));
        SafeRaise(
            PlaybackFailed,
            new PlaybackFailedEventArgs(generation, PlaybackFailureReason.NoVideoTrack));
        return true;
    }

    private void PublishState(PlaybackState state, long? generation = null)
    {
        var eventGeneration = generation ?? Generation;
        var changed = false;

        lock (_stateGate)
        {
            if (eventGeneration < _stateGeneration
                || (generation.HasValue && eventGeneration != Generation))
            {
                return;
            }

            var previousState = (PlaybackState)_stateValue;
            if (previousState != state || _stateGeneration != eventGeneration)
            {
                Volatile.Write(ref _stateValue, (int)state);
                _stateGeneration = eventGeneration;
                changed = true;
            }
        }

        if (changed)
        {
            SafeRaise(
                StateChanged,
                new PlaybackStateChangedEventArgs(state, eventGeneration));
        }
    }

    private static void SafeRaise<TEventArgs>(EventHandler<TEventArgs>? handler, TEventArgs eventArgs)
        where TEventArgs : EventArgs
    {
        if (handler is null)
        {
            return;
        }

        try
        {
            handler.Invoke(null, eventArgs);
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: обробник події завершився помилкою: {exception}");
        }
    }

    private static void TryReleaseNativeResource(Action action, string operation)
    {
        try
        {
            action();
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: помилка {operation}: {exception}");
        }
    }
}

public enum PlaybackState
{
    Idle,
    Opening,
    Playing,
    Paused,
    Stopped,
    Ended,
    Error,
}

public sealed class PlaybackStateChangedEventArgs(PlaybackState state, long generation) : EventArgs
{
    public PlaybackState State { get; } = state;

    public long Generation { get; } = generation;
}

public sealed class PlaybackLengthChangedEventArgs(long length, long generation) : EventArgs
{
    public long Length { get; } = length;

    public long Generation { get; } = generation;
}

public sealed class PlaybackSeekableChangedEventArgs(bool isSeekable, long generation) : EventArgs
{
    public bool IsSeekable { get; } = isSeekable;

    public long Generation { get; } = generation;
}

public sealed class PlaybackVolumeChangedEventArgs(int volume) : EventArgs
{
    public int Volume { get; } = volume;
}

public sealed class PlaybackFailedEventArgs(
    long generation,
    PlaybackFailureReason reason) : EventArgs
{
    public long Generation { get; } = generation;

    public PlaybackFailureReason Reason { get; } = reason;
}

public enum PlaybackFailureReason
{
    Decoding,
    NoVideoTrack,
}
