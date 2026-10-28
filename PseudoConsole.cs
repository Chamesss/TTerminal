using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using static TTerminal.Native;

namespace TTerminal;

// Runs a process attached to a Windows ConPTY
internal sealed class PseudoConsole : IDisposable
{
    private readonly IntPtr _hpc;
    private readonly IntPtr _process;
    private readonly FileStream _input;
    private readonly FileStream _output;
    private bool _disposed;

    public event Action<string>? Output;
    public event Action? Exited;

    public PseudoConsole(string commandLine, string cwd, int cols, int rows)
    {
        if (!CreatePipe(out var inRead, out var inWrite, IntPtr.Zero, 0) ||
            !CreatePipe(out var outRead, out var outWrite, IntPtr.Zero, 0))
            throw new Win32Exception();

        int hr = CreatePseudoConsole(Size(cols, rows), inRead, outWrite, 0, out _hpc);
        inRead.Dispose();
        outWrite.Dispose();
        if (hr != 0) throw new Win32Exception(hr);

        _input = new FileStream(inWrite, FileAccess.Write, 1);
        _output = new FileStream(outRead, FileAccess.Read, 1);
        _process = Spawn(commandLine, cwd);

        new Thread(ReadLoop) { IsBackground = true, Name = "pty-read" }.Start();
        new Thread(() => { WaitForSingleObject(_process, uint.MaxValue); Exited?.Invoke(); }) { IsBackground = true, Name = "pty-wait" }.Start();
    }

    private IntPtr Spawn(string commandLine, string cwd)
    {
        IntPtr size = IntPtr.Zero;
        InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
        IntPtr attrs = Marshal.AllocHGlobal(size);
        try
        {
            if (!InitializeProcThreadAttributeList(attrs, 1, 0, ref size) ||
                !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, _hpc, IntPtr.Size, IntPtr.Zero, IntPtr.Zero))
                throw new Win32Exception();

            var si = new STARTUPINFOEX { lpAttributeList = attrs };
            si.StartupInfo.cb = Marshal.SizeOf<STARTUPINFOEX>();
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

            if (!CreateProcess(null, commandLine, IntPtr.Zero, IntPtr.Zero, false, EXTENDED_STARTUPINFO_PRESENT,
                    IntPtr.Zero, cwd, ref si, out var pi))
                throw new Win32Exception();

            CloseHandle(pi.hThread);
            return pi.hProcess;
        }
        finally
        {
            DeleteProcThreadAttributeList(attrs);
            Marshal.FreeHGlobal(attrs);
        }
    }

    // Streams UTF-8 output, keeping split multi-byte sequences intact
    private void ReadLoop()
    {
        var decoder = Encoding.UTF8.GetDecoder();
        var bytes = new byte[64 * 1024];
        var chars = new char[Encoding.UTF8.GetMaxCharCount(bytes.Length)];
        try
        {
            int n;
            while ((n = _output.Read(bytes, 0, bytes.Length)) > 0)
            {
                int c = decoder.GetChars(bytes, 0, n, chars, 0, false);
                if (c > 0) Output?.Invoke(new string(chars, 0, c));
            }
        }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
    }

    public void Write(string text) => WriteBytes(Encoding.UTF8.GetBytes(text));

    public void WriteBinary(string text) => WriteBytes(Encoding.Latin1.GetBytes(text));

    private void WriteBytes(byte[] data)
    {
        if (_disposed) return;
        try { _input.Write(data); _input.Flush(); }
        catch (IOException) { }
    }

    public void Resize(int cols, int rows)
    {
        if (!_disposed) ResizePseudoConsole(_hpc, Size(cols, rows));
    }

    private static COORD Size(int cols, int rows) =>
        new() { X = (short)Math.Clamp(cols, 1, short.MaxValue), Y = (short)Math.Clamp(rows, 1, short.MaxValue) };

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _input.Dispose();
        ClosePseudoConsole(_hpc);
        CloseHandle(_process);
    }
}
