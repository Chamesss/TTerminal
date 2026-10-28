using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;
using static TTerminal.Native;

namespace TTerminal;

internal sealed class MainForm : Form
{
    private const string Origin = "https://tterminal.example/";

    private readonly WebView2 _web = new() { Dock = DockStyle.Fill, DefaultBackgroundColor = Color.Transparent };
    private readonly StringBuilder _pending = new();
    private readonly object _lock = new();
    private static readonly string DataDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TTerminal");
    private static readonly string SizeFile = Path.Combine(DataDir, "size.txt");

    private readonly string _bash;
    private readonly string _cwd;
    private PseudoConsole? _pty;
    private int _cols, _rows;
    private bool _flushQueued, _ready, _pageReady, _wasMax;

    public MainForm(string bash, string[] args)
    {
        _bash = bash;
        _cwd = args.Length > 0 && Directory.Exists(args[0])
            ? Path.GetFullPath(args[0])
            : Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);

        Text = "TTerminal";
        BackColor = Color.Black;
        StartPosition = FormStartPosition.Manual;
        Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!);
        Controls.Add(_web);
    }

    // Drops WS_SYSMENU so DWM draws no native caption buttons
    protected override CreateParams CreateParams
    {
        get
        {
            var cp = base.CreateParams;
            cp.Style &= ~0x00080000;
            return cp;
        }
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        Size = new Size(LogicalToDeviceUnits(1000), LogicalToDeviceUnits(620));
        MinimumSize = new Size(LogicalToDeviceUnits(420), LogicalToDeviceUnits(220));
        ApplyChrome(Handle);
        StartShell();
        _ = InitAsync();
    }

    // Stays hidden until the page has loaded
    protected override void SetVisibleCore(bool value)
    {
        if (value && !_ready)
        {
            if (!IsHandleCreated) CreateHandle();
            value = false;
        }
        base.SetVisibleCore(value);
    }

    private async Task InitAsync()
    {
        var env = await CoreWebView2Environment.CreateAsync(null, DataDir);
        await _web.EnsureCoreWebView2Async(env);

        var core = _web.CoreWebView2;
        var s = core.Settings;
        s.AreBrowserAcceleratorKeysEnabled = false;
        s.AreDefaultContextMenusEnabled = false;
        s.AreDevToolsEnabled = Debugger.IsAttached;
        s.IsStatusBarEnabled = false;
        s.IsZoomControlEnabled = false;
        s.IsPinchZoomEnabled = false;
        s.IsSwipeNavigationEnabled = false;
        s.IsNonClientRegionSupportEnabled = true;

        core.AddWebResourceRequestedFilter(Origin + "*", CoreWebView2WebResourceContext.All);
        core.WebResourceRequested += (_, e) => e.Response = Serve(env, e.Request.Uri);
        core.WebMessageReceived += OnMessage;
        core.NavigationCompleted += (_, _) =>
        {
            if (_ready) return;
            _ready = true;
            CenterToScreen();
            Show();
            Activate();
        };
        core.Navigate(Origin + "index.html");
    }

    // Serves the embedded web assets
    private static CoreWebView2WebResourceResponse Serve(CoreWebView2Environment env, string uri)
    {
        var name = "web/" + new Uri(uri).AbsolutePath.TrimStart('/');
        var stream = typeof(MainForm).Assembly.GetManifestResourceStream(name);
        if (stream is null) return env.CreateWebResourceResponse(null, 404, "Not Found", "");

        var mime = Path.GetExtension(name) switch
        {
            ".html" => "text/html",
            ".css" => "text/css",
            ".js" => "text/javascript",
            _ => "application/octet-stream",
        };
        return env.CreateWebResourceResponse(stream, 200, "OK", $"Content-Type: {mime}; charset=utf-8");
    }

    private void OnMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        var msg = e.TryGetWebMessageAsString();
        if (string.IsNullOrEmpty(msg)) return;
        var arg = msg[1..];

        switch (msg[0])
        {
            case 'k': OnPageReady(arg); break;
            case 'i': _pty?.Write(arg); break;
            case 'b': _pty?.WriteBinary(arg); break;
            case 'r': ResizeShell(arg); break;
            case 'c': SetClipboard(arg); break;
            case 'p': Post("p" + GetClipboard()); break;
            case 't': Text = string.IsNullOrWhiteSpace(arg) ? "TTerminal" : arg; break;
            case 'u': OpenLink(arg); break;
            case 'n': Process.Start(Environment.ProcessPath!, $"\"{_cwd}\""); break;
            case 'm': WindowState = FormWindowState.Minimized; break;
            case 'x': WindowState = WindowState == FormWindowState.Maximized ? FormWindowState.Normal : FormWindowState.Maximized; break;
            case 'q': Close(); break;
            case 's': if (int.TryParse(arg, out var edge)) BeginResize(edge); break;
        }
    }

    // Starts bash at launch, in parallel with the page load
    private void StartShell()
    {
        Environment.SetEnvironmentVariable("TERM", "xterm-256color");
        Environment.SetEnvironmentVariable("COLORTERM", "truecolor");
        Environment.SetEnvironmentVariable("TERM_PROGRAM", "TTerminal");
        Environment.SetEnvironmentVariable("CHERE_INVOKING", "1");

        (_cols, _rows) = LoadSize();
        _pty = new PseudoConsole($"\"{_bash}\" --login -i", _cwd, _cols, _rows);
        _pty.Output += OnOutput;
        _pty.Exited += () => Ui(Close);
    }

    // Syncs the real terminal size, then releases buffered output
    private void OnPageReady(string size)
    {
        ResizeShell(size);
        _pageReady = true;
        Flush();
    }

    private void ResizeShell(string size)
    {
        if (!TryParseSize(size, out var cols, out var rows) || (cols == _cols && rows == _rows)) return;
        (_cols, _rows) = (cols, rows);
        _pty?.Resize(cols, rows);
    }

    private static (int, int) LoadSize()
    {
        try { if (TryParseSize(File.ReadAllText(SizeFile), out var c, out var r)) return (c, r); }
        catch (IOException) { }
        return (115, 33);
    }

    private void SaveSize()
    {
        try { Directory.CreateDirectory(DataDir); File.WriteAllText(SizeFile, $"{_cols},{_rows}"); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    // Coalesces output chunks into one message per UI tick
    private void OnOutput(string text)
    {
        bool schedule;
        lock (_lock)
        {
            _pending.Append(text);
            schedule = !_flushQueued;
            _flushQueued = true;
        }
        if (schedule) Ui(Flush);
    }

    private void Flush()
    {
        string text;
        lock (_lock)
        {
            if (!_pageReady)
            {
                _flushQueued = false;
                return;
            }
            text = _pending.ToString();
            _pending.Clear();
            _flushQueued = false;
        }
        if (text.Length > 0) Post("o" + text);
    }

    private void Post(string msg)
    {
        if (_web.CoreWebView2 is { } core && !IsDisposed) core.PostWebMessageAsString(msg);
    }

    private void Ui(Action action)
    {
        if (IsDisposed || !IsHandleCreated) return;
        try { BeginInvoke(action); } catch (InvalidOperationException) { }
    }

    private void BeginResize(int edge)
    {
        if (WindowState != FormWindowState.Normal) return;
        ReleaseCapture();
        SendMessage(Handle, WM_SYSCOMMAND, SC_SIZE | edge, IntPtr.Zero);
    }

    private static bool TryParseSize(string s, out int cols, out int rows)
    {
        cols = rows = 0;
        var parts = s.Split(',');
        return parts.Length == 2 && int.TryParse(parts[0], out cols) && int.TryParse(parts[1], out rows);
    }

    private static void SetClipboard(string text)
    {
        if (text.Length == 0) return;
        try { Clipboard.SetText(text); } catch (ExternalException) { }
    }

    private static string GetClipboard()
    {
        try { return Clipboard.ContainsText() ? Clipboard.GetText() : ""; }
        catch (ExternalException) { return ""; }
    }

    private static void OpenLink(string uri)
    {
        if (Uri.TryCreate(uri, UriKind.Absolute, out var u) && (u.Scheme == Uri.UriSchemeHttp || u.Scheme == Uri.UriSchemeHttps))
            Process.Start(new ProcessStartInfo(u.AbsoluteUri) { UseShellExecute = true });
    }

    protected override void OnActivated(EventArgs e)
    {
        base.OnActivated(e);
        Post("f1");
        _web.Focus();
    }

    protected override void OnDeactivate(EventArgs e)
    {
        base.OnDeactivate(e);
        Post("f0");
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        bool max = WindowState == FormWindowState.Maximized;
        if (max == _wasMax) return;
        _wasMax = max;
        Post(max ? "m1" : "m0");
    }

    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        _pty?.Dispose();
        if (WindowState == FormWindowState.Normal) SaveSize();
        base.OnFormClosed(e);
    }

    // Removes the native title bar, keeps invisible resize borders
    protected override void WndProc(ref Message m)
    {
        if (m.Msg == WM_NCCALCSIZE && m.WParam != IntPtr.Zero)
        {
            int top = Marshal.ReadInt32(m.LParam, 4);
            base.WndProc(ref m);
            if (IsZoomed(Handle)) top += FrameThickness(Handle);
            Marshal.WriteInt32(m.LParam, 4, top);
            m.Result = IntPtr.Zero;
            return;
        }
        base.WndProc(ref m);
    }
}
