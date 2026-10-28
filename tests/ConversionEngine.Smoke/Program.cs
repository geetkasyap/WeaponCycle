using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ImageMagick;

var portable = args.Length >= 1 ? Path.GetFullPath(args[0])
    : Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, "out", "portable"));
var helper = Path.Combine(portable, "conversion", "WeaponCycle.Conversion.exe");
if (!File.Exists(helper))
{
    throw new FileNotFoundException("Build the complete portable converter first.", helper);
}

var root = Path.Combine(Path.GetTempPath(), "WeaponCycle-Smoke-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);

async Task<JsonElement> Run(string[] arguments, bool cancel = false)
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

    using var process = Process.Start(start) ?? throw new IOException("Converter did not start.");
    var output = process.StandardOutput.ReadToEndAsync();
    var errors = process.StandardError.ReadToEndAsync();
    if (cancel)
    {
        await process.StandardInput.WriteLineAsync("cancel");
        await process.StandardInput.FlushAsync();
    }
    try
    {
        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromMinutes(2));
    }
    catch (TimeoutException) { process.Kill(entireProcessTree: true); throw; }
    var messages = (await output).Split('\n', StringSplitOptions.RemoveEmptyEntries)
        .Where(line => line.StartsWith('{')).Select(line => JsonDocument.Parse(line).RootElement.Clone())
        .Where(message => message.GetProperty("type").GetString() is "formats" or "result").ToList();
    if (messages.Count != 1)
    {
        throw new InvalidDataException($"Invalid converter response: {await errors}\n{await output}");
    }

    return messages[0];
}

string Hash(string file) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file)));
void Check(bool condition, string error)
{
    if (!condition)
    {
        throw new InvalidDataException(error);
    }
}

async Task<string> ConvertFile(string source, string to)
{
    var before = Hash(source);
    var timer = Stopwatch.StartNew();
    var result = await Run(["convert", source, to]);
    Check(result.GetProperty("succeeded").GetBoolean(), $"{Path.GetFileName(source)} -> {to}: {result}");
    var outputs = result.GetProperty("outputs").EnumerateArray().Select(p => p.GetString()!).ToList();
    Check(outputs.Count > 0, "No output was returned.");
    foreach (var path in outputs)
    {
        Check(File.Exists(path) && new FileInfo(path).Length > 0 || Directory.Exists(path), "Missing/empty output: " + path);
    }

    Check(Hash(source) == before, "Original was modified: " + source);
    Console.WriteLine($"PASS {Path.GetFileName(source)} -> {to} ({timer.Elapsed.TotalSeconds:F2}s)");
    return outputs[0];
}

string DocxText(string file)
{
    using var zip = ZipFile.OpenRead(file);
    using var stream = (zip.GetEntry("word/document.xml") ?? throw new InvalidDataException("Invalid DOCX")).Open();
    using var reader = new StreamReader(stream);
    return reader.ReadToEnd();
}

try
{
    HostChecks.Run(root);
    CatalogChecks.Run();
    if (args.Length == 3 && args[1] == "--benchmark")
    {
        await ConversionBenchmark.RunAsync(helper, root, Path.GetFullPath(args[2]));
        return;
    }
    ResourceLimits.Thread = 2;
    var png = Path.Combine(root, "photo with spaces.png");
    var jpg = Path.Combine(root, "photo with spaces.jpg");
    using (var image = new MagickImage(MagickColors.CornflowerBlue, 96, 64))
    {
        image.Write(png);
        image.Write(jpg);
    }
    var formats = await Run(["inspect", jpg]);
    Check(formats.GetProperty("type").GetString() == "formats" &&
        formats.GetProperty("targets").EnumerateArray().Any(t =>
            t.GetProperty("id").GetString() == "png" && t.GetProperty("available").GetBoolean()),
        "JPG did not offer an available PNG conversion.");
    var converted = await ConvertFile(jpg, "png");
    using (var image = new MagickImage(converted))
    {
        Check(image.Width == 96 && image.Height == 64, "JPG -> PNG changed dimensions.");
    }

    Check(await ConvertFile(jpg, "png") != converted, "Second conversion overwrote an existing result.");
    await ConvertFile(png, "jpg");
    var webp = await ConvertFile(png, "webp");
    await ConvertFile(webp, "png");
    var heic = await ConvertFile(png, "heic");
    var heicBack = await ConvertFile(heic, "png");
    using (var image = new MagickImage(heicBack))
    {
        Check(image.Width == 96 && image.Height == 64, "HEIC -> PNG changed dimensions.");
    }

    var text = Path.Combine(root, "notes.txt");
    await File.WriteAllTextAsync(text,
        "Weapon Cycle converter test.\nThis sentence must survive Word and PDF conversions.");
    var word = await ConvertFile(text, "docx");
    Check(DocxText(word).Contains("sentence"), "Generated Word file lost text.");
    var pdf = await ConvertFile(word, "pdf");
    Check(Encoding.ASCII.GetString(File.ReadAllBytes(pdf), 0, 5) == "%PDF-", "Word -> PDF did not write a PDF.");
    var wordBack = await ConvertFile(pdf, "docx");
    Check(DocxText(wordBack).Contains("sentence"), "PDF -> Word lost searchable text.");
    var pdfText = await ConvertFile(pdf, "txt");
    Check((await File.ReadAllTextAsync(pdfText)).Contains("sentence"), "PDF text could not be read back.");
    await ConvertFile(pdf, "png");

    var csv = Path.Combine(root, "table.csv");
    await File.WriteAllTextAsync(csv, "Name,Count\r\nAlpha,12\r\nBeta,34\r\n");
    var sheet = await ConvertFile(csv, "xlsx");
    var csvBack = await ConvertFile(sheet, "csv");
    Check((await File.ReadAllTextAsync(csvBack)).Contains("Alpha"), "Spreadsheet conversion lost data.");
    var json = Path.Combine(root, "data.json");
    await File.WriteAllTextAsync(json, "{\"name\":\"Weapon Cycle\",\"count\":12}");
    var yaml = await ConvertFile(json, "yaml");
    await ConvertFile(yaml, "json");
    var archive = await ConvertFile(text, "zip");
    await ConvertFile(archive, "folder");

    var wav = Path.Combine(root, "tone.wav");
    using (var writer = new BinaryWriter(File.Create(wav)))
    {
        writer.Write("RIFF"u8);
        writer.Write(32036);
        writer.Write("WAVEfmt "u8);
        writer.Write(16);
        writer.Write((short)1);
        writer.Write((short)1);
        writer.Write(16000);
        writer.Write(32000);
        writer.Write((short)2);
        writer.Write((short)16);
        writer.Write("data"u8);
        writer.Write(32000);
        for (var i = 0; i < 16000; i++)
        {
            writer.Write((short)(4000 * Math.Sin(2 * Math.PI * 440 * i / 16000)));
        }
    }
    var mp3 = await ConvertFile(wav, "mp3");
    await ConvertFile(mp3, "flac");
    var invalid = Path.Combine(root, "broken.pdf");
    await File.WriteAllTextAsync(invalid, "This is not a PDF.");
    var failure = await Run(["convert", invalid, "docx"]);
    Check(!failure.GetProperty("succeeded").GetBoolean() &&
        !string.IsNullOrWhiteSpace(failure.GetProperty("error").GetString()), "Corrupt PDF was not reported as a failure.");
    var cancelled = await Run(["convert", word, "pdf"], cancel: true);
    Check(!cancelled.GetProperty("succeeded").GetBoolean() && cancelled.GetProperty("cancelled").GetBoolean(),
        "Cancellation was not honored.");
    await ScenarioChecks.RunAsync(root, png, pdf, Run, ConvertFile);
    Check(!Directory.EnumerateDirectories(root, "*.partial", SearchOption.AllDirectories).Any(),
        "A job left a partial output folder.");
    Console.WriteLine("PASS original preservation, collision safety, corruption errors, cancellation and cleanup.");
}
finally
{
    Directory.Delete(root, recursive: true);
}
