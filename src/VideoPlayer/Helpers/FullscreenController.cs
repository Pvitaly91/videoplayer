using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace VideoPlayer.Helpers;

public sealed class FullscreenController
{
    private const uint MonitorDefaultToNearest = 2;
    private const uint SwpNoSize = 0x0001;
    private const uint SwpNoMove = 0x0002;
    private const uint SwpFrameChanged = 0x0020;
    private const uint SwpNoActivate = 0x0010;
    private const uint SwpShowWindow = 0x0040;
    private static readonly nint HwndTopmost = new(-1);
    private static readonly nint HwndNotTopmost = new(-2);

    private readonly Window _window;
    private WindowSnapshot? _snapshot;

    public FullscreenController(Window window)
    {
        _window = window ?? throw new ArgumentNullException(nameof(window));
    }

    public bool IsFullscreen => _snapshot is not null;

    public void Toggle()
    {
        if (IsFullscreen)
        {
            Exit();
        }
        else
        {
            Enter();
        }
    }

    public void Enter()
    {
        if (IsFullscreen)
        {
            return;
        }

        var windowHandle = new WindowInteropHelper(_window).Handle;
        if (windowHandle == 0)
        {
            throw new InvalidOperationException("Вікно ще не готове до переходу в повноекранний режим.");
        }

        var monitorHandle = MonitorFromWindow(windowHandle, MonitorDefaultToNearest);
        var monitorInfo = new MonitorInfo
        {
            Size = (uint)Marshal.SizeOf<MonitorInfo>(),
        };

        if (monitorHandle == 0 || !GetMonitorInfo(monitorHandle, ref monitorInfo))
        {
            throw new InvalidOperationException("Не вдалося визначити поточний монітор.");
        }

        _snapshot = new WindowSnapshot(
            _window.RestoreBounds,
            _window.WindowStyle,
            _window.ResizeMode,
            _window.WindowState,
            _window.Topmost);

        _window.WindowState = WindowState.Normal;
        _window.WindowStyle = WindowStyle.None;
        _window.ResizeMode = ResizeMode.NoResize;
        _window.Topmost = true;

        var monitorBounds = monitorInfo.Monitor;
        if (!SetWindowPos(
                windowHandle,
                HwndTopmost,
                monitorBounds.Left,
                monitorBounds.Top,
                monitorBounds.Right - monitorBounds.Left,
                monitorBounds.Bottom - monitorBounds.Top,
                SwpFrameChanged | SwpNoActivate | SwpShowWindow))
        {
            Exit();
            throw new InvalidOperationException("Не вдалося розгорнути вікно на весь монітор.");
        }
    }

    public void Exit()
    {
        if (_snapshot is not { } snapshot)
        {
            return;
        }

        _snapshot = null;
        var windowHandle = new WindowInteropHelper(_window).Handle;

        _window.WindowState = WindowState.Normal;
        _window.Topmost = snapshot.Topmost;
        _window.WindowStyle = snapshot.WindowStyle;
        _window.ResizeMode = snapshot.ResizeMode;

        if (!snapshot.RestoreBounds.IsEmpty)
        {
            _window.Left = snapshot.RestoreBounds.Left;
            _window.Top = snapshot.RestoreBounds.Top;
            _window.Width = snapshot.RestoreBounds.Width;
            _window.Height = snapshot.RestoreBounds.Height;
        }

        if (windowHandle != 0)
        {
            _ = SetWindowPos(
                windowHandle,
                snapshot.Topmost ? HwndTopmost : HwndNotTopmost,
                0,
                0,
                0,
                0,
                SwpNoSize | SwpNoMove | SwpFrameChanged | SwpNoActivate);
        }

        _window.WindowState = snapshot.WindowState;
    }

    public void PrepareForClose()
    {
        _snapshot = null;
    }

    [DllImport("user32.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    private static extern nint MonitorFromWindow(nint windowHandle, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(nint monitorHandle, ref MonitorInfo monitorInfo);

    [DllImport("user32.dll", SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        nint windowHandle,
        nint insertAfter,
        int x,
        int y,
        int width,
        int height,
        uint flags);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MonitorInfo
    {
        public uint Size;
        public NativeRect Monitor;
        public NativeRect WorkArea;
        public uint Flags;
    }

    private sealed record WindowSnapshot(
        Rect RestoreBounds,
        WindowStyle WindowStyle,
        ResizeMode ResizeMode,
        WindowState WindowState,
        bool Topmost);
}
