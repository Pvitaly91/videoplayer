using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;
using VideoPlayer.Helpers;
using VideoPlayer.Services;

namespace VideoPlayer;

public partial class MainWindow : Window
{
    private const string ApplicationTitle = "VideoPlayer";
    private const string VideoFileFilter =
        "Відеофайли|*.mp4;*.avi;*.mkv;*.mov;*.m4v;*.webm;*.wmv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.flv;*.3gp;*.ogv;*.vob|Усі файли|*.*";

    private readonly PlaybackService _playbackService;
    private readonly IReadOnlyList<string> _startupArguments;
    private readonly DispatcherTimer _uiTimer;
    private FullscreenController? _fullscreenController;
    private long _mediaGeneration;
    private long _duration;
    private long _timelineUpdatesSuppressedUntil;
    private bool _isSeekable;
    private bool _isSeeking;
    private bool _isAdjustingVolume;
    private bool _isUpdatingTimeline;
    private bool _isUpdatingVolume;
    private bool _isClosing;
    private bool _startupArgumentProcessed;
    private bool _playbackErrorShown;
    private bool _videoSurfaceAttached;

    public MainWindow(PlaybackService playbackService, IReadOnlyList<string> startupArguments)
    {
        _playbackService = playbackService ?? throw new ArgumentNullException(nameof(playbackService));
        _startupArguments = startupArguments ?? throw new ArgumentNullException(nameof(startupArguments));

        InitializeComponent();

        _fullscreenController = new FullscreenController(this);
        _playbackService.MediaPlayer.EnableKeyInput = false;
        _playbackService.MediaPlayer.EnableMouseInput = false;

        ProgressSlider.AddHandler(
            Thumb.DragStartedEvent,
            new DragStartedEventHandler(ProgressSlider_DragStarted));
        ProgressSlider.AddHandler(
            Thumb.DragCompletedEvent,
            new DragCompletedEventHandler(ProgressSlider_DragCompleted));
        ProgressSlider.AddHandler(
            MouseLeftButtonDownEvent,
            new MouseButtonEventHandler(ProgressSlider_MouseLeftButtonDown),
            handledEventsToo: true);
        ProgressSlider.AddHandler(
            Mouse.LostMouseCaptureEvent,
            new MouseEventHandler(ProgressSlider_LostMouseCapture),
            handledEventsToo: true);
        VolumeSlider.AddHandler(
            Mouse.LostMouseCaptureEvent,
            new MouseEventHandler(VolumeSlider_LostMouseCapture),
            handledEventsToo: true);
        AddHandler(
            PreviewMouseLeftButtonDownEvent,
            new MouseButtonEventHandler(Window_PreviewMouseLeftButtonDown),
            handledEventsToo: true);
        AddHandler(
            PreviewMouseLeftButtonUpEvent,
            new MouseButtonEventHandler(Window_PreviewMouseLeftButtonUp),
            handledEventsToo: true);

        _playbackService.StateChanged += PlaybackService_StateChanged;
        _playbackService.LengthChanged += PlaybackService_LengthChanged;
        _playbackService.SeekableChanged += PlaybackService_SeekableChanged;
        _playbackService.VolumeChanged += PlaybackService_VolumeChanged;
        _playbackService.PlaybackFailed += PlaybackService_PlaybackFailed;

        _uiTimer = new DispatcherTimer(DispatcherPriority.Background, Dispatcher)
        {
            Interval = TimeSpan.FromMilliseconds(250),
        };
        _uiTimer.Tick += UiTimer_Tick;

        UpdateVolumeUi(_playbackService.Volume);
        UpdateControlAvailability();
    }

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        EnsureVideoSurfaceReady();

        if (_startupArgumentProcessed)
        {
            return;
        }

        _startupArgumentProcessed = true;
        var startupFile = CommandLineFileResolver.ResolveFirstExistingFile(_startupArguments);
        if (startupFile is not null)
        {
            OpenFile(startupFile);
            return;
        }

        if (_startupArguments.Count > 0)
        {
            MessageBox.Show(
                "Не вдалося відкрити файл із командного рядка. Передайте повний шлях до наявного локального файла.",
                "VideoPlayer — файл не знайдено",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
        }
    }

    private void OpenFileButton_Click(object sender, RoutedEventArgs e)
    {
        ShowOpenFileDialog();
    }

    private void PlayPauseButton_Click(object sender, RoutedEventArgs e)
    {
        RunPlaybackAction(_playbackService.TogglePlayPause);
    }

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        RunPlaybackAction(_playbackService.Stop);
        UpdateTimeline(0);
    }

    private void MuteButton_Click(object sender, RoutedEventArgs e)
    {
        RunPlaybackAction(_playbackService.ToggleMute);
    }

    private void FullscreenButton_Click(object sender, RoutedEventArgs e)
    {
        ToggleFullscreen();
    }

    private void Window_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = TryGetDroppedPath(e.Data) is not null
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private void Window_Drop(object sender, DragEventArgs e)
    {
        var filePath = TryGetDroppedPath(e.Data);
        if (filePath is not null)
        {
            OpenFile(filePath);
        }

        e.Handled = true;
        Activate();
        Focus();
    }

    private void VideoSurface_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left && e.ClickCount == 2 && _playbackService.HasMedia)
        {
            ToggleFullscreen();
            e.Handled = true;
        }

        Activate();
        Focus();
    }

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        var modifiers = Keyboard.Modifiers;

        if (e.Key == Key.O && modifiers.HasFlag(ModifierKeys.Control) && !e.IsRepeat)
        {
            ShowOpenFileDialog();
            e.Handled = true;
            return;
        }

        if (e.Key == Key.Escape && _fullscreenController?.IsFullscreen == true)
        {
            ExitFullscreen();
            e.Handled = true;
            return;
        }

        if ((e.Key == Key.F || e.Key == Key.F11)
            && modifiers == ModifierKeys.None
            && !e.IsRepeat)
        {
            if (_playbackService.HasMedia)
            {
                ToggleFullscreen();
                e.Handled = true;
            }

            return;
        }

        if (_isSeeking || _isAdjustingVolume)
        {
            return;
        }

        if (!_playbackService.HasMedia)
        {
            return;
        }

        switch (e.Key)
        {
            case Key.Space when modifiers == ModifierKeys.None && !e.IsRepeat:
                RunPlaybackAction(_playbackService.TogglePlayPause);
                e.Handled = true;
                break;

            case Key.Left when modifiers is ModifierKeys.None or ModifierKeys.Control:
                SeekBy(modifiers.HasFlag(ModifierKeys.Control) ? -30 : -5);
                e.Handled = true;
                break;

            case Key.Right when modifiers is ModifierKeys.None or ModifierKeys.Control:
                SeekBy(modifiers.HasFlag(ModifierKeys.Control) ? 30 : 5);
                e.Handled = true;
                break;

            case Key.Up when modifiers == ModifierKeys.None:
                ChangeVolume(5);
                e.Handled = true;
                break;

            case Key.Down when modifiers == ModifierKeys.None:
                ChangeVolume(-5);
                e.Handled = true;
                break;

            case Key.M when modifiers == ModifierKeys.None && !e.IsRepeat:
                RunPlaybackAction(_playbackService.ToggleMute);
                e.Handled = true;
                break;
        }
    }

    private void Window_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (ProgressSlider.IsEnabled && ProgressSlider.IsMouseOver)
        {
            _isSeeking = true;
        }

        if (VolumeSlider.IsEnabled && VolumeSlider.IsMouseOver)
        {
            _isAdjustingVolume = true;
        }
    }

    private void ProgressSlider_LostMouseCapture(object sender, MouseEventArgs e)
    {
        CommitSeek();
    }

    private void ProgressSlider_DragStarted(object sender, DragStartedEventArgs e)
    {
        _isSeeking = true;
    }

    private void ProgressSlider_DragCompleted(object sender, DragCompletedEventArgs e)
    {
        CommitSeek();
    }

    private void ProgressSlider_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (IsWithinThumb(e.OriginalSource as DependencyObject))
        {
            return;
        }

        // Slider's class handler moves the value for a track click. Commit after the
        // routed event completes so this one-shot interaction cannot leave drag state set.
        Dispatcher.BeginInvoke(DispatcherPriority.Input, CommitSeek);
    }

    private void Window_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        CommitSeek();
        _isAdjustingVolume = false;
    }

    private void VolumeSlider_LostMouseCapture(object sender, MouseEventArgs e)
    {
        _isAdjustingVolume = false;
    }

    private void ProgressSlider_ValueChanged(
        object sender,
        RoutedPropertyChangedEventArgs<double> e)
    {
        if (_isUpdatingTimeline)
        {
            return;
        }

        if (_isSeeking)
        {
            CurrentTimeText.Text = TimeFormatter.Format((long)e.NewValue);
            return;
        }

        if (ProgressSlider.IsKeyboardFocusWithin && ProgressSlider.IsEnabled)
        {
            var requestedPosition = (long)e.NewValue;
            if (RunPlaybackAction(() => _playbackService.SeekTo(requestedPosition)))
            {
                SuppressTimelineUpdatesAfterSeek();
            }
            else
            {
                UpdateTimeline(_playbackService.CurrentTime);
            }
        }
    }

    private void VolumeSlider_ValueChanged(
        object sender,
        RoutedPropertyChangedEventArgs<double> e)
    {
        if (_isUpdatingVolume)
        {
            return;
        }

        var volume = PlaybackMath.ClampVolume((int)Math.Round(e.NewValue));
        if (VolumeValueText is not null)
        {
            VolumeValueText.Text = $"{volume}%";
        }

        if (_playbackService.HasMedia)
        {
            RunPlaybackAction(() => _playbackService.SetVolume(volume));
        }
    }

    private void PlaybackService_StateChanged(object? sender, PlaybackStateChangedEventArgs e)
    {
        DispatchToUi(() =>
        {
            if (!TryAcceptGeneration(e.Generation, out var isNewGeneration))
            {
                return;
            }

            if (isNewGeneration)
            {
                ResetTimelineForNewMedia();
            }

            if (e.Generation != _playbackService.Generation
                || e.State != _playbackService.State)
            {
                return;
            }

            ApplyPlaybackState(e.State);
        });
    }

    private void PlaybackService_LengthChanged(object? sender, PlaybackLengthChangedEventArgs e)
    {
        DispatchToUi(() =>
        {
            if (!TryAcceptGeneration(e.Generation, out var isNewGeneration))
            {
                return;
            }

            if (isNewGeneration)
            {
                ResetTimelineForNewMedia();
            }

            SetDuration(e.Length);
        });
    }

    private void PlaybackService_SeekableChanged(object? sender, PlaybackSeekableChangedEventArgs e)
    {
        DispatchToUi(() =>
        {
            if (!TryAcceptGeneration(e.Generation, out var isNewGeneration))
            {
                return;
            }

            if (isNewGeneration)
            {
                ResetTimelineForNewMedia();
            }

            _isSeekable = e.IsSeekable;
            UpdateControlAvailability();
        });
    }

    private void PlaybackService_VolumeChanged(object? sender, PlaybackVolumeChangedEventArgs e)
    {
        DispatchToUi(() => UpdateVolumeUi(e.Volume));
    }

    private void PlaybackService_PlaybackFailed(object? sender, PlaybackFailedEventArgs e)
    {
        DispatchToUi(() =>
        {
            if (!TryAcceptGeneration(e.Generation, out var isNewGeneration))
            {
                return;
            }

            if (isNewGeneration)
            {
                ResetTimelineForNewMedia();
            }

            if (_playbackService.State != PlaybackState.Error)
            {
                return;
            }

            if (e.Reason == PlaybackFailureReason.NoVideoTrack)
            {
                RunPlaybackAction(_playbackService.Stop);
            }

            ShowPlaybackError();
        });
    }

    private void UiTimer_Tick(object? sender, EventArgs e)
    {
        if (!_playbackService.HasMedia)
        {
            return;
        }

        var observedLength = _playbackService.Length;
        if (observedLength != _duration)
        {
            SetDuration(observedLength);
        }

        if (!_isSeeking && Environment.TickCount64 >= _timelineUpdatesSuppressedUntil)
        {
            UpdateTimeline(_playbackService.CurrentTime);
        }
    }

    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (_isClosing)
        {
            return;
        }

        _isClosing = true;
        _uiTimer.Stop();
        _uiTimer.Tick -= UiTimer_Tick;

        _playbackService.StateChanged -= PlaybackService_StateChanged;
        _playbackService.LengthChanged -= PlaybackService_LengthChanged;
        _playbackService.SeekableChanged -= PlaybackService_SeekableChanged;
        _playbackService.VolumeChanged -= PlaybackService_VolumeChanged;
        _playbackService.PlaybackFailed -= PlaybackService_PlaybackFailed;

        _fullscreenController?.PrepareForClose();
        _fullscreenController = null;

        try
        {
            VideoView.MediaPlayer = null;
            VideoView.Dispose();
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: помилка від'єднання відеоповерхні: {exception}");
        }

        try
        {
            _playbackService.Dispose();
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: помилка звільнення LibVLC: {exception}");
        }
    }

    private void ShowOpenFileDialog()
    {
        var dialog = new OpenFileDialog
        {
            Title = "Відкрити відеофайл",
            Filter = VideoFileFilter,
            FilterIndex = 1,
            CheckFileExists = true,
            CheckPathExists = true,
            Multiselect = false,
        };

        if (dialog.ShowDialog(this) == true)
        {
            OpenFile(dialog.FileName);
        }
    }

    private void OpenFile(string filePath)
    {
        if (string.IsNullOrWhiteSpace(filePath) || !File.Exists(filePath))
        {
            MessageBox.Show(
                "Файл не знайдено або він недоступний.",
                "VideoPlayer — не вдалося відкрити файл",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            return;
        }

        _playbackErrorShown = false;
        VideoView.Visibility = Visibility.Visible;
        EmptyMessageText.Visibility = Visibility.Collapsed;
        VideoView.UpdateLayout();

        try
        {
            _playbackService.OpenAndPlay(filePath);
            Title = $"{Path.GetFileName(filePath)} — {ApplicationTitle}";
            _uiTimer.Start();
            UpdateControlAvailability();
        }
        catch (Exception exception) when (exception is DllNotFoundException or BadImageFormatException)
        {
            var retainedExistingMedia = _playbackService.HasMedia;
            ResetEmptyStateIfNeeded();
            ShowNativeLibVlcError();
            if (retainedExistingMedia)
            {
                _playbackErrorShown = false;
            }
        }
        catch (Exception exception) when (exception is IOException
                                              or UnauthorizedAccessException
                                              or ArgumentException
                                              or InvalidOperationException)
        {
            Debug.WriteLine($"VideoPlayer: не вдалося відкрити файл: {exception}");
            var retainedExistingMedia = _playbackService.HasMedia;
            ResetEmptyStateIfNeeded();
            ShowPlaybackError();
            if (retainedExistingMedia)
            {
                _playbackErrorShown = false;
            }
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: неочікувана помилка відкриття файла: {exception}");
            var retainedExistingMedia = _playbackService.HasMedia;
            ResetEmptyStateIfNeeded();
            ShowPlaybackError();
            if (retainedExistingMedia)
            {
                _playbackErrorShown = false;
            }
        }
    }

    private void RunPlaybackAction(Action action)
    {
        try
        {
            action();
        }
        catch (ObjectDisposedException) when (_isClosing)
        {
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: помилка керування відтворенням: {exception}");
            ShowPlaybackError();
        }
    }

    private bool RunPlaybackAction(Func<bool> action)
    {
        try
        {
            return action();
        }
        catch (ObjectDisposedException) when (_isClosing)
        {
            return false;
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: помилка керування відтворенням: {exception}");
            ShowPlaybackError();
            return false;
        }
    }

    private void ApplyPlaybackState(PlaybackState state)
    {
        switch (state)
        {
            case PlaybackState.Playing:
                PlayPauseButton.Content = "⏸";
                PlayPauseButton.ToolTip = "Пауза (Space)";
                break;

            case PlaybackState.Opening:
            case PlaybackState.Paused:
            case PlaybackState.Stopped:
            case PlaybackState.Ended:
            case PlaybackState.Error:
            case PlaybackState.Idle:
                PlayPauseButton.Content = "▶";
                PlayPauseButton.ToolTip = "Відтворити (Space)";
                break;

            default:
                throw new ArgumentOutOfRangeException(nameof(state), state, null);
        }

        if (state is PlaybackState.Stopped or PlaybackState.Ended)
        {
            UpdateTimeline(0);
        }

        UpdateControlAvailability();
    }

    private void SetDuration(long duration)
    {
        _duration = Math.Max(0, duration);
        _isUpdatingTimeline = true;
        try
        {
            ProgressSlider.Maximum = Math.Max(1, _duration);
            TotalTimeText.Text = TimeFormatter.Format(_duration);
        }
        finally
        {
            _isUpdatingTimeline = false;
        }

        UpdateControlAvailability();
    }

    private void UpdateTimeline(long position)
    {
        var clampedPosition = PlaybackMath.ClampPosition(position, _duration);
        _isUpdatingTimeline = true;
        try
        {
            ProgressSlider.Value = clampedPosition;
            CurrentTimeText.Text = TimeFormatter.Format(clampedPosition);
        }
        finally
        {
            _isUpdatingTimeline = false;
        }
    }

    private void CommitSeek()
    {
        if (!_isSeeking)
        {
            return;
        }

        _isSeeking = false;
        var requestedPosition = PlaybackMath.ClampPosition(
            (long)ProgressSlider.Value,
            _duration);
        if (RunPlaybackAction(() => _playbackService.SeekTo(requestedPosition)))
        {
            SuppressTimelineUpdatesAfterSeek();
            UpdateTimeline(requestedPosition);
        }
        else
        {
            UpdateTimeline(_playbackService.CurrentTime);
        }
    }

    private void SeekBy(int offsetSeconds)
    {
        if (!_isSeekable || !ProgressSlider.IsEnabled)
        {
            return;
        }

        var target = PlaybackMath.CalculateSeekTarget(
            (long)ProgressSlider.Value,
            offsetSeconds,
            _duration);
        if (RunPlaybackAction(() => _playbackService.SeekTo(target)))
        {
            SuppressTimelineUpdatesAfterSeek();
            UpdateTimeline(target);
        }
    }

    private void ChangeVolume(int delta)
    {
        RunPlaybackAction(() => _playbackService.SetVolume(_playbackService.Volume + delta));
    }

    private void UpdateVolumeUi(int volume)
    {
        var clampedVolume = PlaybackMath.ClampVolume(volume);
        _isUpdatingVolume = true;
        try
        {
            VolumeSlider.Value = clampedVolume;
            VolumeValueText.Text = $"{clampedVolume}%";
            MuteButton.Content = clampedVolume == 0 ? "🔇" : "🔊";
            MuteButton.ToolTip = clampedVolume == 0
                ? "Увімкнути звук (M)"
                : "Вимкнути звук (M)";
        }
        finally
        {
            _isUpdatingVolume = false;
        }
    }

    private void UpdateControlAvailability()
    {
        var hasMedia = _playbackService.HasMedia;
        PlayPauseButton.IsEnabled = hasMedia;
        StopButton.IsEnabled = hasMedia;
        MuteButton.IsEnabled = hasMedia;
        VolumeSlider.IsEnabled = hasMedia;
        FullscreenButton.IsEnabled = hasMedia;
        ProgressSlider.IsEnabled = hasMedia
                                   && _isSeekable
                                   && _duration > 0
                                   && _playbackService.State is PlaybackState.Playing or PlaybackState.Paused;
    }

    private bool TryAcceptGeneration(long generation, out bool isNewGeneration)
    {
        isNewGeneration = generation > _mediaGeneration;
        if (generation < _mediaGeneration)
        {
            return false;
        }

        if (isNewGeneration)
        {
            _mediaGeneration = generation;
        }

        return true;
    }

    private void ResetTimelineForNewMedia()
    {
        _playbackErrorShown = false;
        _duration = 0;
        _isSeekable = false;
        _isSeeking = false;
        _timelineUpdatesSuppressedUntil = 0;
        SetDuration(0);
        UpdateTimeline(0);
    }

    private void SuppressTimelineUpdatesAfterSeek()
    {
        _timelineUpdatesSuppressedUntil = Environment.TickCount64 + 500;
    }

    private static bool IsWithinThumb(DependencyObject? source)
    {
        for (var current = source; current is not null; current = VisualTreeHelper.GetParent(current))
        {
            if (current is Thumb)
            {
                return true;
            }
        }

        return false;
    }

    private void ToggleFullscreen()
    {
        try
        {
            _fullscreenController?.Toggle();
            UpdateFullscreenUi();
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: помилка повноекранного режиму: {exception}");
            MessageBox.Show(
                "Не вдалося перемкнути повноекранний режим.",
                "VideoPlayer",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
        }
    }

    private void ExitFullscreen()
    {
        try
        {
            _fullscreenController?.Exit();
            UpdateFullscreenUi();
        }
        catch (Exception exception)
        {
            Debug.WriteLine($"VideoPlayer: помилка виходу з повноекранного режиму: {exception}");
        }
    }

    private void UpdateFullscreenUi()
    {
        FullscreenButton.ToolTip = _fullscreenController?.IsFullscreen == true
            ? "Вийти з повноекранного режиму (Escape)"
            : "Повноекранний режим (F або F11)";
    }

    private void ResetEmptyStateIfNeeded()
    {
        if (_playbackService.HasMedia)
        {
            return;
        }

        Title = ApplicationTitle;
        _uiTimer.Stop();
        VideoView.Visibility = Visibility.Hidden;
        EmptyMessageText.Visibility = Visibility.Visible;
        _duration = 0;
        _isSeekable = false;
        SetDuration(0);
        UpdateTimeline(0);
        UpdateControlAvailability();
    }

    private void ShowPlaybackError()
    {
        if (_playbackErrorShown || _isClosing)
        {
            return;
        }

        _playbackErrorShown = true;
        MessageBox.Show(
            "Не вдалося відтворити файл. Файл може бути пошкоджений, не містити відеодоріжки або містити непідтримуваний кодек.",
            "VideoPlayer — помилка відтворення",
            MessageBoxButton.OK,
            MessageBoxImage.Error);
    }

    private void EnsureVideoSurfaceReady()
    {
        if (_videoSurfaceAttached)
        {
            return;
        }

        _ = VideoView.ApplyTemplate();
        VideoView.UpdateLayout();
        VideoView.MediaPlayer = _playbackService.MediaPlayer;
        _videoSurfaceAttached = true;
    }

    private void ShowNativeLibVlcError()
    {
        if (_playbackErrorShown || _isClosing)
        {
            return;
        }

        _playbackErrorShown = true;
        MessageBox.Show(
            "Не знайдено нативні компоненти LibVLC. Не видаляйте файли libvlc.dll, libvlccore.dll і каталог plugins із portable-збірки.",
            "VideoPlayer — помилка LibVLC",
            MessageBoxButton.OK,
            MessageBoxImage.Error);
    }

    private void DispatchToUi(Action action)
    {
        if (_isClosing || Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
        {
            return;
        }

        if (Dispatcher.CheckAccess())
        {
            action();
            return;
        }

        try
        {
            _ = Dispatcher.BeginInvoke(DispatcherPriority.DataBind, new Action(() =>
            {
                if (!_isClosing)
                {
                    action();
                }
            }));
        }
        catch (InvalidOperationException exception)
        {
            Debug.WriteLine($"VideoPlayer: UI dispatcher уже завершив роботу: {exception.Message}");
        }
    }

    private static string? TryGetDroppedPath(IDataObject data)
    {
        if (!data.GetDataPresent(DataFormats.FileDrop)
            || data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } files)
        {
            return null;
        }

        return string.IsNullOrWhiteSpace(files[0]) ? null : files[0];
    }
}
