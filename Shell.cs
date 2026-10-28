namespace TTerminal;

internal static class Shell
{
    // Resolves bash: TTERMINAL_SHELL, Git Bash, MSYS2, then PATH
    public static string? FindBash()
    {
        var pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        var pf86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);

        string?[] candidates =
        [
            Environment.GetEnvironmentVariable("TTERMINAL_SHELL"),
            Path.Combine(pf, @"Git\bin\bash.exe"),
            Path.Combine(pf86, @"Git\bin\bash.exe"),
            Path.Combine(local, @"Programs\Git\bin\bash.exe"),
            @"C:\msys64\usr\bin\bash.exe",
            .. (Environment.GetEnvironmentVariable("PATH") ?? "")
                .Split(';', StringSplitOptions.RemoveEmptyEntries)
                .Select(d => Path.Combine(d.Trim(), "bash.exe")),
        ];

        return candidates.FirstOrDefault(p => !string.IsNullOrEmpty(p) && File.Exists(p));
    }
}
