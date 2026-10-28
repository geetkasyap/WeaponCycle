using System.Text.Json;
using WeaponCycle.Conversion;

internal static class HostChecks
{
    public static void Run(string root)
    {
        ConversionRequest inspect = ConversionRequest.Parse(["inspect", "sample.png"]);
        ConversionRequest convert = ConversionRequest.Parse(["convert", "sample.png", "jpg"]);
        ConversionRequest install = ConversionRequest.Parse(["install", "ffmpeg"]);
        Check(inspect.Command == ConversionCommand.Inspect && inspect.Input == "sample.png",
            "Inspect arguments changed.");
        Check(convert.Command == ConversionCommand.Convert && convert.TargetFormat == "jpg",
            "Convert arguments changed.");
        Check(install.Command == ConversionCommand.Install && install.Input == "ffmpeg",
            "Install arguments changed.");
        foreach (string[] invalid in new string[][]
        {
            [], ["unknown", "sample.png"], ["convert", "sample.png"],
            ["inspect", "sample.png", "extra"], ["inspect", ""], ["convert", "sample.png", " "]
        })
        {
            Expect<ArgumentException>(() => ConversionRequest.Parse(invalid));
        }

        using var writer = new StringWriter();
        var messages = new ConversionMessages(writer);
        messages.Progress(0.5, "Working");
        messages.Formats(new ConversionFormatsMessage(
            "PNG", "Image", [new ConversionTargetMessage("jpg", "JPG", true, [], 0)]));
        messages.Result(new ConversionResultMessage(true, false, [@"C:\result.jpg"]));
        JsonElement[] serialized = writer.ToString().Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Select(line => JsonDocument.Parse(line).RootElement.Clone()).ToArray();
        Check(serialized.Length == 3 &&
            serialized[0].GetProperty("type").GetString() == "progress" &&
            serialized[0].GetProperty("progress").GetDouble() == 0.5 &&
            serialized[1].GetProperty("type").GetString() == "formats" &&
            serialized[1].GetProperty("targets")[0].GetProperty("available").GetBoolean() &&
            serialized[2].GetProperty("type").GetString() == "result" &&
            serialized[2].GetProperty("succeeded").GetBoolean() &&
            serialized[2].GetProperty("error").GetString() == "",
            "The native JSON-lines contract changed.");

        string source = Path.Combine(root, "workspace-source.txt");
        File.WriteAllText(source, "Keep the original.");
        string existing = Path.Combine(root, "workspace-source (converted)");
        Directory.CreateDirectory(existing);
        File.WriteAllText(Path.Combine(existing, "marker.txt"), "Keep the previous output.");
        string publishedFile;
        string staging;
        using (var workspace = new OutputWorkspace(source))
        {
            staging = workspace.DirectoryPath;
            string output = Path.Combine(staging, "document.txt");
            File.WriteAllText(output, "Complete output.");
            Directory.CreateDirectory(Path.Combine(staging, "assets"));
            File.WriteAllText(Path.Combine(staging, "assets", "image.txt"), "Companion data.");
            publishedFile = workspace.Publish([output], CancellationToken.None).Single();
            Check(File.Exists(publishedFile) && !Directory.Exists(staging),
                "Publication did not move the complete workspace.");
            Check(File.Exists(Path.Combine(Path.GetDirectoryName(publishedFile)!, "assets", "image.txt")),
                "Publication lost companion files.");
            Expect<InvalidOperationException>(() =>
                workspace.Publish([publishedFile], CancellationToken.None));
        }
        Check(File.ReadAllText(source) == "Keep the original." &&
            File.ReadAllText(Path.Combine(existing, "marker.txt")) == "Keep the previous output." &&
            File.ReadAllText(publishedFile) == "Complete output.",
            "Publication changed existing data.");

        using (var workspace = new OutputWorkspace(source))
        {
            staging = workspace.DirectoryPath;
            Expect<InvalidDataException>(() => workspace.Publish([source], CancellationToken.None));
            string missing = Path.Combine(staging, "missing.txt");
            Expect<InvalidDataException>(() => workspace.Publish([missing], CancellationToken.None));
            string unfinished = Path.Combine(staging, "partial.txt");
            File.WriteAllText(unfinished, "Do not publish.");
            using var cancellation = new CancellationTokenSource();
            cancellation.Cancel();
            Expect<OperationCanceledException>(() => workspace.Publish([unfinished], cancellation.Token));
        }
        Check(!Directory.Exists(staging), "Cancelled work was not removed.");

        var disposed = new OutputWorkspace(source);
        staging = disposed.DirectoryPath;
        disposed.Dispose();
        Directory.CreateDirectory(staging);
        File.WriteAllText(Path.Combine(staging, "external.txt"), "Not owned by the disposed workspace.");
        disposed.Dispose();
        Check(File.Exists(Path.Combine(staging, "external.txt")),
            "Repeated disposal removed data it no longer owned.");
        Directory.Delete(staging, recursive: true);
        Console.WriteLine("PASS command parsing, typed protocol and output-workspace ownership checks.");
    }

    private static void Expect<TException>(Action action) where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException)
        {
            return;
        }
        throw new InvalidDataException("Expected " + typeof(TException).Name + ".");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidDataException(message);
        }
    }
}
