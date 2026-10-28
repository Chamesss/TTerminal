namespace TTerminal;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();

        var bash = Shell.FindBash();
        if (bash is null)
        {
            MessageBox.Show("bash.exe not found.\nInstall Git for Windows or set TTERMINAL_SHELL.", "TTerminal",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        Application.Run(new MainForm(bash, args));
    }
}
