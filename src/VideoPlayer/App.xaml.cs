using System.IO;
using System.Windows;
using LibVLCSharp.Shared;
using VideoPlayer.Services;

namespace VideoPlayer;

public partial class App : Application
{
    private PlaybackService? _playbackService;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        try
        {
            Core.Initialize();

            _playbackService = new PlaybackService();
            var mainWindow = new MainWindow(_playbackService, e.Args);
            MainWindow = mainWindow;
            mainWindow.Show();
        }
        catch (Exception exception) when (IsNativeLibVlcFailure(exception))
        {
            DisposePlaybackService();
            MessageBox.Show(
                "Не вдалося завантажити нативні компоненти LibVLC. Вони можуть бути відсутні або мати несумісну версію. Переконайтеся, що всю portable-папку скопійовано повністю разом із каталогами libvlc та plugins.",
                "VideoPlayer — помилка LibVLC",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            Shutdown(-1);
        }
        catch (Exception exception)
        {
            DisposePlaybackService();
            MessageBox.Show(
                $"Не вдалося запустити VideoPlayer.\n\n{exception.Message}",
                "VideoPlayer — помилка запуску",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            Shutdown(-1);
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        DisposePlaybackService();
        base.OnExit(e);
    }

    private static bool IsNativeLibVlcFailure(Exception exception)
    {
        for (Exception? current = exception; current is not null; current = current.InnerException)
        {
            if (current is DllNotFoundException
                or BadImageFormatException
                or EntryPointNotFoundException
                or VLCException)
            {
                return true;
            }

            if (current is FileNotFoundException fileNotFoundException
                && fileNotFoundException.FileName?.Contains(
                    "libvlc",
                    StringComparison.OrdinalIgnoreCase) == true)
            {
                return true;
            }
        }

        return false;
    }

    private void DisposePlaybackService()
    {
        _playbackService?.Dispose();
        _playbackService = null;
    }
}
