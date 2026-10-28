using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using ImageMagick;

internal static class ConversionBenchmark
{
    public static async Task RunAsync(string helper, string root, string outputFile)
    {
        Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.BelowNormal;
        ResourceLimits.Thread = 2;
        var measurements = new List<object>();
        var png = Path.Combine(root, "benchmark.png");
        var jpg = Path.Combine(root, "benchmark.jpg");
        using (var image = new MagickImage("gradient:#204060-#e0a040", 1920, 1080))
        {
            image.Write(png);
            image.Write(jpg);
        }
        var text = Path.Combine(root, "benchmark.txt");
        await File.WriteAllTextAsync(text, "Weapon Cycle bounded conversion benchmark.\nText must remain searchable.");
        var word = "";
        var pdf = "";
        foreach (var (source, target, name) in new[]
        {
            (jpg, "png", "jpg-to-png-2mp"),
            (png, "jpg", "png-to-jpg-2mp"),
            (text, "docx", "text-to-docx")
        })
        {
            var result = await Measure(source, target, name);
            if (target == "docx")
            {
                word = result;
            }
        }
        pdf = await Measure(word, "pdf", "docx-to-pdf");
        await Measure(pdf, "docx", "pdf-to-docx");
        foreach (var source in new[] { jpg, word })
        {
            var values = new List<double>();
            for (var i = 0; i < 5; i++)
            {
                var (message, elapsed, cpu) = await Invoke(["inspect", source]);
                if (message.GetProperty("type").GetString() != "formats")
                {
                    throw new InvalidDataException("Format discovery failed: " + message);
                }

                values.Add(elapsed);
                measurements.Add(new
                {
                    scenario = "inspect-" + Path.GetExtension(source),
                    run = i,
                    milliseconds = elapsed,
                    cpuMilliseconds = cpu
                });
                await Task.Delay(100);
            }
            Console.WriteLine($"inspect {Path.GetExtension(source)}: median {Median(values):F1} ms");
        }
        await File.WriteAllTextAsync(outputFile, JsonSerializer.Serialize(measurements, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine("Saved bounded benchmark: " + outputFile);

        async Task<string> Measure(string source, string target, string name)
        {
            var hash = SHA256.HashData(await File.ReadAllBytesAsync(source));
            var values = new List<double>();
            string last = "";
            for (var i = 0; i < 5; i++)
            {
                var (message, elapsed, cpu) = await Invoke(["convert", source, target]);
                if (!message.GetProperty("succeeded").GetBoolean())
                {
                    throw new InvalidDataException(name + ": " + message);
                }

                last = message.GetProperty("outputs")[0].GetString()!;
                if (!File.Exists(last) || new FileInfo(last).Length == 0)
                {
                    throw new InvalidDataException("Benchmark output is missing.");
                }

                var currentHash = SHA256.HashData(await File.ReadAllBytesAsync(source));
                if (!hash.SequenceEqual(currentHash))
                {
                    throw new InvalidDataException("Benchmark modified its original.");
                }

                if (target is "png" or "jpg")
                {
                    using var image = new MagickImage(last);
                    if (image.Width != 1920 || image.Height != 1080)
                    {
                        throw new InvalidDataException("Benchmark image dimensions changed.");
                    }
                }
                values.Add(elapsed);
                measurements.Add(new
                {
                    scenario = name,
                    run = i,
                    milliseconds = elapsed,
                    cpuMilliseconds = cpu
                });
                await Task.Delay(100);
            }
            Console.WriteLine($"{name}: median {Median(values):F1} ms");
            return last;
        }

        async Task<(JsonElement, double, double)> Invoke(string[] arguments)
        {
            var start = new ProcessStartInfo(helper)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            foreach (var argument in arguments)
            {
                start.ArgumentList.Add(argument);
            }

            var timer = Stopwatch.StartNew();
            using var process = Process.Start(start) ?? throw new IOException("Benchmark helper could not start.");
            var stdout = process.StandardOutput.ReadToEndAsync();
            var stderr = process.StandardError.ReadToEndAsync();
            try
            {
                await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(45));
            }
            catch (TimeoutException) { process.Kill(entireProcessTree: true); throw; }
            timer.Stop();
            var terminal = (await stdout).Split('\n', StringSplitOptions.RemoveEmptyEntries)
                .Where(line => line.StartsWith('{'))
                .Select(line => JsonDocument.Parse(line).RootElement.Clone())
                .SingleOrDefault(value => value.GetProperty("type").GetString() is "result" or "formats");
            if (terminal.ValueKind != JsonValueKind.Object || process.ExitCode != 0)
            {
                throw new InvalidDataException($"Benchmark helper failed: {await stderr}\n{terminal}");
            }

            return (terminal, timer.Elapsed.TotalMilliseconds, process.TotalProcessorTime.TotalMilliseconds);
        }
    }

    private static double Median(List<double> values) => values.Order().ElementAt(values.Count / 2);
}
