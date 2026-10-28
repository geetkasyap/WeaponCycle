using System.IO.Compression;
using System.Text.Json;
using Filee.Core.Conversion;
using Filee.Core.Presets;
using ImageMagick;
using PdfSharp.Pdf;
using PdfSharp.Pdf.IO;

internal static class ScenarioChecks
{
    public static async Task RunAsync(
        string root, string png, string pdf,
        Func<string[], bool, Task<JsonElement>> run,
        Func<string, string, Task<string>> convert)
    {
        void Check(bool condition, string error)
        {
            if (!condition)
            {
                throw new InvalidDataException(error);
            }
        }
        async Task Refused(string source, string target, string scenario)
        {
            var result = await run(["convert", source, target], false);
            Check(!result.GetProperty("succeeded").GetBoolean() &&
                !string.IsNullOrWhiteSpace(result.GetProperty("error").GetString()),
                "A failure was hidden: " + scenario);
            Check(result.GetProperty("outputs").GetArrayLength() == 0, "A failure returned completed outputs: " + scenario);
            Console.WriteLine("PASS " + scenario);
        }
        await Refused(Path.Combine(root, "missing.png"), "png", "missing input");
        var empty = Path.Combine(root, "empty.png");
        await File.WriteAllBytesAsync(empty, []);
        await Refused(empty, "png", "empty input");
        await Refused(root, "png", "folder input");
        await Refused(png, "not-a-format", "invalid target");
        var spoofed = Path.Combine(root, "spoofed.png");
        await File.WriteAllTextAsync(spoofed, "This is not an image.");
        await Refused(spoofed, "jpg", "invalid image bytes");
        using (var locked = new FileStream(png, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            await Refused(png, "jpg", "input locked by another application");
        }

        var unicode = Path.Combine(root, "\u65e5\u672c\u8a9e image.PNG");
        File.Copy(png, unicode);
        await convert(unicode, "jpg");
        var readOnly = Path.Combine(root, "read-only.png");
        File.Copy(png, readOnly);
        File.SetAttributes(readOnly, FileAttributes.ReadOnly);
        try
        {
            await convert(readOnly, "webp");
        }
        finally { File.SetAttributes(readOnly, FileAttributes.Normal); }

        var collision = Path.Combine(root, "collision.png");
        File.Copy(png, collision);
        var marker = Path.Combine(root, "collision (converted)");
        await File.WriteAllTextAsync(marker, "Preserve this existing file.");
        await convert(collision, "jpg");
        Check(await File.ReadAllTextAsync(marker) == "Preserve this existing file.", "An output-name collision replaced user data.");
        Console.WriteLine("PASS file/directory output-name collision");

        var alpha = Path.Combine(root, "alpha.png");
        using (var image = new MagickImage(new MagickColor("#4488CC80"), 40, 30))
        {
            image.Write(alpha);
        }

        var alphaTiff = await convert(alpha, "tiff");
        using (var image = new MagickImage(alphaTiff))
        {
            using var pixels = image.GetPixels();
            using var original = new MagickImage(alpha);
            using var sourcePixels = original.GetPixels();
            var actual = pixels.GetPixel(0, 0).ToColor()!.A;
            var expected = sourcePixels.GetPixel(0, 0).ToColor()!.A;
            Check(image.HasAlpha && Math.Abs(actual - expected) <= 1,
                $"Transparency changed: source alpha {expected}, TIFF alpha {actual}, alpha channel {image.HasAlpha}.");
            Check(pixels.ToByteArray(PixelMapping.RGBA)!.Take(4)
                    .SequenceEqual(sourcePixels.ToByteArray(PixelMapping.RGBA)!.Take(4)),
                "Palette PNG color samples changed during TIFF encoding.");
        }
        var upright = Path.Combine(root, "rotated.tiff");
        using (var image = new MagickImage(MagickColors.DarkOrange, 40, 20))
        {
            var exif = new ExifProfile();
            exif.SetValue(ExifTag.Orientation, (ushort)6);
            image.SetProfile(exif);
            image.Orientation = OrientationType.RightTop;
            image.Write(upright);
        }
        var oriented = await convert(upright, "png");
        using (var image = new MagickImage(oriented))
        {
            Check(image.Width == 20 && image.Height == 40, "EXIF rotation was not applied.");
        }

        var pages = Path.Combine(root, "two-pages.tiff");
        using (var images = new MagickImageCollection())
        {
            images.Add(new MagickImage(MagickColors.Red, 40, 30));
            images.Add(new MagickImage(MagickColors.Blue, 40, 30));
            images.Write(pages);
        }
        var pageResults = await run(["convert", pages, "png"], false);
        Check(pageResults.GetProperty("succeeded").GetBoolean() &&
            pageResults.GetProperty("outputs").GetArrayLength() == 2, "Multipage TIFF lost pages.");
        Console.WriteLine("PASS multipage TIFF emits both pages");

        var animation = Path.Combine(root, "animated.gif");
        using (var frames = new MagickImageCollection())
        {
            frames.Add(new MagickImage(MagickColors.Red, 32, 24) { AnimationDelay = 20 });
            frames.Add(new MagickImage(MagickColors.Blue, 32, 24) { AnimationDelay = 20 });
            frames.Write(animation);
        }
        var webp = await convert(animation, "webp");
        using (var frames = new MagickImageCollection(webp))
        {
            Check(frames.Count == 2, "Animation lost a frame.");
        }

        var multiPdf = Path.Combine(root, "two-pages.pdf");
        using (var source = PdfReader.Open(pdf, PdfDocumentOpenMode.Import))
        using (var output = new PdfDocument())
        {
            output.AddPage(source.Pages[0]);
            output.AddPage(source.Pages[0]);
            output.Save(multiPdf);
        }
        var pdfResults = await run(["convert", multiPdf, "png"], false);
        Check(pdfResults.GetProperty("succeeded").GetBoolean() &&
            pdfResults.GetProperty("outputs").GetArrayLength() == 2, "PDF page conversion lost a page.");
        Console.WriteLine("PASS PDF emits both pages");
        var protectedPdf = Path.Combine(root, "protected.pdf");
        using (var document = PdfReader.Open(pdf, PdfDocumentOpenMode.Modify))
        {
            document.SecuritySettings.UserPassword = "test-fixture-only";
            document.SecuritySettings.OwnerPassword = "test-owner-only";
            document.Save(protectedPdf);
        }
        await Refused(protectedPdf, "docx", "password-protected PDF");
        var slideDeck = await convert(pdf, "pptx");
        using (var slides = ZipFile.OpenRead(slideDeck))
        {
            Check(slides.GetEntry("ppt/slides/slide1.xml") is not null, "PDF -> PPTX did not create a slide.");
        }

        await convert(slideDeck, "pdf");

        var imagePdf = await convert(png, "pdf");
        var reconstructed = await convert(imagePdf, "docx");
        using (var document = ZipFile.OpenRead(reconstructed))
        {
            Check(document.Entries.Any(e => e.FullName.StartsWith("word/media/")),
                "An image-only PDF lost its page image.");
        }

        var archive = Path.Combine(root, "broken.zip");
        await File.WriteAllTextAsync(archive, "Not an archive.");
        await Refused(archive, "folder", "corrupt archive");
        var hostile = Path.Combine(root, "escaping.zip");
        using (var zip = ZipFile.Open(hostile, ZipArchiveMode.Create))
        using (var writer = new StreamWriter(zip.CreateEntry("../must-not-escape.txt").Open()))
        {
            writer.Write("Must never leave the extraction directory.");
        }

        var extraction = await run(["convert", hostile, "folder"], false);
        Check(!File.Exists(Path.Combine(root, "must-not-escape.txt")) &&
            !File.Exists(Path.Combine(Path.GetDirectoryName(root)!, "must-not-escape.txt")),
            "Archive extraction escaped its output directory.");
        Check(!extraction.GetProperty("succeeded").GetBoolean(), "Unsafe archive was reported as converted.");
        Console.WriteLine("PASS archive path-traversal refusal");

        Check(!WeaponCycle.Conversion.ConversionRequest.AcceptsInputLength(0) &&
            WeaponCycle.Conversion.ConversionRequest.AcceptsInputLength(1) &&
            WeaponCycle.Conversion.ConversionRequest.AcceptsInputLength(2L * 1024 * 1024 * 1024) &&
            !WeaponCycle.Conversion.ConversionRequest.AcceptsInputLength(2L * 1024 * 1024 * 1024 + 1),
            "The 2 GiB boundary is incorrect.");
        Console.WriteLine("PASS exact input-size boundary without creating a multi-gigabyte fixture");
        await CheckActiveCancellation(root, png);
        Check(!Directory.EnumerateDirectories(root, "*.partial", SearchOption.AllDirectories).Any(),
            "A failure left a partial output directory.");
        Console.WriteLine("PASS scenario matrix cleanup");
    }

    private static async Task CheckActiveCancellation(string root, string source)
    {
        var output = Path.Combine(root, "controlled-cancel");
        Directory.CreateDirectory(output);
        var converter = new CancellableConverter();
        await using var queue = new JobQueue(new ConverterCatalog([converter]));
        var job = new ConversionJob([source], new Preset
        {
            TargetFormat = "png",
            Output = new OutputRule { Location = OutputLocation.CustomFolder, CustomFolder = output }
        }, "controlled cancellation");
        var running = queue.RunAsync(job);
        try
        {
            await converter.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            job.Cancel();
            await running.WaitAsync(TimeSpan.FromSeconds(5));
            if (job.State != JobState.Cancelled || File.Exists(converter.PartialOutput))
            {
                throw new InvalidDataException("An active cancelled job kept an incomplete output.");
            }
        }
        finally
        {
            job.Cancel();
            await running.WaitAsync(TimeSpan.FromSeconds(5));
        }
        Console.WriteLine("PASS cancellation after output writing starts, without a stress workload");
    }

    private sealed class CancellableConverter : IConverter
    {
        public string Id => "cancellation-fixture";
        public string DisplayName => Id;
        public int MaxParallelism => 1;
        public IReadOnlyList<ConversionEdge> Edges { get; } = [new("png", "png")];
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public string PartialOutput { get; private set; } = "";
        public EngineStatus GetStatus() => EngineStatus.Available();
        public async Task<IReadOnlyList<string>> ConvertAsync(
            ConversionStep step, IProgress<double>? progress, CancellationToken cancellationToken)
        {
            PartialOutput = step.Output.Allocate("png") ?? throw new IOException("Cannot allocate the cancellation fixture.");
            await File.WriteAllTextAsync(PartialOutput, "incomplete", cancellationToken);
            Started.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            throw new InvalidOperationException("The fixture must be cancelled.");
        }
    }
}
