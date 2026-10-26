using System.Diagnostics;
using System.Security.Cryptography;
using Filee.Engines.Infrastructure;

namespace WeaponCycle.Conversion;

internal static class ArchiveEngineInstall
{
    // Filee's optional installer handles Office MSIs, not its bundled 7-Zip MSI.
    public static async Task InstallAsync(CancellationToken cancellationToken)
    {
        var component = EngineDownloads.Components["7zip"];
        var destination = Path.Combine(EngineEnvironment.DownloadRoot, "7zip");
        var marker = Path.Combine(destination, ".filee-component");
        if (File.Exists(marker) && File.ReadAllText(marker) == component.Sha256 &&
            File.Exists(Path.Combine(destination, "7z.exe")) && File.Exists(Path.Combine(destination, "7z.dll")))
        {
            return;
        }

        var staging = Path.Combine(EngineEnvironment.DownloadRoot, ".7zip-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(staging);
        try
        {
            using (var download = new HttpClient { MaxResponseContentBufferSize = 32 * 1024 * 1024 })
            {
                var bytes = await download.GetByteArrayAsync(component.Url, cancellationToken);
                if (!string.Equals(Convert.ToHexString(SHA256.HashData(bytes)), component.Sha256,
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException("7-Zip checksum mismatch; nothing was installed.");
                }

                await File.WriteAllBytesAsync(Path.Combine(staging, "engine.msi"), bytes, cancellationToken);
            }

            var unpacked = Path.Combine(staging, "unpacked");
            var start = new ProcessStartInfo(
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "msiexec.exe"),
                $"/a \"{Path.Combine(staging, "engine.msi")}\" /qn TARGETDIR=\"{unpacked}\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true
            };
            using var process = Process.Start(start) ?? throw new IOException("7-Zip extraction could not start.");
            try
            {
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                timeout.CancelAfter(TimeSpan.FromMinutes(3));
                await process.WaitForExitAsync(timeout.Token);
            }
            catch (OperationCanceledException)
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                }

                await process.WaitForExitAsync(CancellationToken.None);
                throw;
            }
            if (process.ExitCode != 0)
            {
                throw new IOException($"7-Zip extraction failed ({process.ExitCode}).");
            }

            var executable = Directory.EnumerateFiles(unpacked, "7z.exe", SearchOption.AllDirectories).Single();
            var programs = Path.GetDirectoryName(executable)!;
            var ready = Path.Combine(staging, "ready");
            Directory.CreateDirectory(ready);
            foreach (var name in new[] { "7z.exe", "7z.dll", "License.txt", "readme.txt" })
            {
                File.Copy(Path.Combine(programs, name), Path.Combine(ready, name));
            }

            File.WriteAllText(Path.Combine(ready, ".filee-component"), component.Sha256);
            cancellationToken.ThrowIfCancellationRequested();
            if (Directory.Exists(destination))
            {
                throw new IOException("A 7-Zip engine folder already exists. It was left unchanged.");
            }

            Directory.Move(ready, destination);
        }
        finally
        {
            if (Directory.Exists(staging))
            {
                Directory.Delete(staging, recursive: true);
            }
        }
    }
}
