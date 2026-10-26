using System.Diagnostics;
using System.Text.Json;
using Filee.Core.Conversion;
using Filee.Core.Formats;
using Filee.Core.Presets;
using Filee.Engines;
using Filee.Engines.Infrastructure;

namespace WeaponCycle.Conversion;

internal sealed class ConversionHost(ConversionMessages messages, string dataDirectory)
{
    private static readonly IReadOnlyList<EnginePackage> s_packages =
    [
        new("rhwp", ["rhwp", "vcruntime"], 80_000_000, ["rhwp"]),
        new("7zip", ["7zip"], 6_000_000, ["archive"]),
        .. EngineDownloads.Packages
    ];
    private readonly ConversionMessages _messages = messages;
    private readonly string _dataDirectory = dataDirectory;

    public async Task<int> RunAsync(ConversionRequest request, CancellationToken cancellationToken)
    {
        if (request.Command == ConversionCommand.Install)
        {
            await InstallAsync(request.Input, cancellationToken);
            return 0;
        }

        string sourcePath = Path.GetFullPath(request.Input);
        ValidateSource(sourcePath);
        FileFormat sourceFormat = FormatRegistry.Detect(sourcePath)
            ?? throw new ArgumentException(
                "This file type is not supported. Choose a real image, document, media, archive or data file.");
        FileFormat? target = request.Command == ConversionCommand.Convert
            ? FormatRegistry.FindById(request.TargetFormat!) : null;
        if (request.Command == ConversionCommand.Convert && (target is null || !target.Writable))
        {
            throw new ArgumentException("Choose a supported output format.");
        }

        _messages.Progress(0, "Checking local conversion engines...");
        IReadOnlyList<IConverter> converters =
            EngineRegistry.CreateAll(new EngineEnvironment(_dataDirectory));
        if (request.Command == ConversionCommand.Inspect)
        {
            _messages.Formats(DiscoverFormats(sourceFormat, converters, cancellationToken));
            return 0;
        }
        ConversionResultMessage result = await ConvertAsync(
            sourcePath, sourceFormat, target!, converters, cancellationToken);
        _messages.Result(result);
        return result.Succeeded ? 0 : 1;
    }

    private static void ValidateSource(string path)
    {
        if (!File.Exists(path) || new FileInfo(path).Length == 0)
        {
            throw new ArgumentException("Choose an existing, non-empty file, not a folder.");
        }
        if (!ConversionRequest.AcceptsInputLength(new FileInfo(path).Length))
        {
            throw new ArgumentException("The interactive converter accepts files up to 2 GiB.");
        }
    }

    private static bool PacksArchive(FileFormat source, FileFormat target) =>
        source.Category != FormatCategory.Archive && target.Id is "zip" or "7z" or "tar" or "tgz";

    private ConversionFormatsMessage DiscoverFormats(
        FileFormat source, IReadOnlyList<IConverter> converters, CancellationToken cancellationToken)
    {
        var catalog = new ConverterCatalog(converters);
        RoutePlanner installed = catalog.CreatePlanner();
        RoutePlanner possible = catalog.CreatePlanner(s_packages.Where(EngineDownloads.CanDownload)
            .SelectMany(package => package.ConverterIds).ToArray());
        IConverter archive = converters.Single(converter => converter.Id == "archive");
        var targets = new List<ConversionTargetMessage>();
        foreach (FileFormat target in FormatRegistry.Known.Where(format => format.Writable))
        {
            cancellationToken.ThrowIfCancellationRequested();
            Route? route = installed.Plan(source.Id, target.Id);
            Route? future = route ?? possible.Plan(source.Id, target.Id);
            if (PacksArchive(source, target))
            {
                future = new Route([new RouteStep(archive, source.Id, target.Id)], 10);
                route = catalog.StatusOf(archive).IsAvailable ? future : null;
            }
            if (future is null)
            {
                continue;
            }
            EnginePackage[] missing = future.Steps
                .Where(step => !catalog.StatusOf(step.Converter).IsAvailable)
                .Select(step => s_packages.FirstOrDefault(
                    package => package.ConverterIds.Contains(step.Converter.Id)))
                .OfType<EnginePackage>().DistinctBy(package => package.Id).ToArray();
            if (route is null && missing.Length == 0)
            {
                continue;
            }
            targets.Add(new ConversionTargetMessage(
                target.Id, target.DisplayName, route is not null,
                missing.Select(package => package.Id).ToArray(),
                missing.Sum(EngineDownloads.DownloadSize)));
        }
        return new ConversionFormatsMessage(source.DisplayName, source.Category.ToString(), targets);
    }

    private async Task<ConversionResultMessage> ConvertAsync(
        string sourcePath, FileFormat source, FileFormat target,
        IReadOnlyList<IConverter> allConverters, CancellationToken cancellationToken)
    {
        bool packArchive = PacksArchive(source, target);
        IReadOnlyList<IConverter> converters = ConversionCatalog.ForConversion(
            allConverters, source.Id, target.Id, packArchive);
        var catalog = new ConverterCatalog(converters);
        RoutePlanner planner = catalog.CreatePlanner();
        bool available = packArchive
            ? converters.Any(converter => converter is IFileCombiner &&
                catalog.StatusOf(converter).IsAvailable)
            : planner.Plan(source.Id, target.Id) is not null;
        if (!available)
        {
            throw new ArgumentException(
                "That conversion is not ready. Select its format and add the required engine first.");
        }
        cancellationToken.ThrowIfCancellationRequested();

        using var workspace = new OutputWorkspace(sourcePath);
        var preset = new Preset
        {
            TargetFormat = target.Id,
            Output = new OutputRule
            {
                Location = OutputLocation.CustomFolder,
                CustomFolder = workspace.DirectoryPath,
                FileNamePattern = "{name} (converted)",
                Conflict = ConflictPolicy.Rename,
                ReplaceOriginal = false
            }
        };
        preset.Archive.CombineIntoOne = packArchive;
        await using var queue = new JobQueue(
            catalog, EngineRegistry.FindPdfMerger(converters),
            combiner: EngineRegistry.FindFileCombiner(converters),
            tiffMerger: EngineRegistry.FindTiffMerger(converters));
        var job = new ConversionJob([sourcePath], preset, target.DisplayName);
        using var cancelJob = cancellationToken.Register(job.Cancel);
        var lastProgress = Stopwatch.StartNew();
        queue.JobUpdated += (_, updated) =>
        {
            if (lastProgress.ElapsedMilliseconds < 100)
            {
                return;
            }
            lastProgress.Restart();
            _messages.Progress(
                updated.Progress, $"Converting {source.DisplayName} to {target.DisplayName}...");
        };
        await queue.RunAsync(job);

        FileResult file = job.Files.Single();
        string[] outputs = file.Outputs.Where(
            path => File.Exists(path) || Directory.Exists(path)).ToArray();
        bool success = file.State == FileState.Done && outputs.Length > 0 &&
            !cancellationToken.IsCancellationRequested;
        outputs = success ? workspace.Publish(outputs, cancellationToken) : [];
        string error = success ? "" : ErrorText(file.ErrorKey);
        if (!success && !string.IsNullOrWhiteSpace(file.ErrorDetail))
        {
            error += " " + file.ErrorDetail;
        }
        return new ConversionResultMessage(
            success,
            !success && (file.State == FileState.Cancelled || cancellationToken.IsCancellationRequested),
            outputs, error, ResultNotice(file, source, target));
    }

    private async Task InstallAsync(string packageId, CancellationToken cancellationToken)
    {
        EnginePackage package = s_packages.SingleOrDefault(package => package.Id == packageId)
            ?? throw new ArgumentException("Unknown conversion engine.");
        if (package.Id == "7zip")
        {
            _messages.Progress(0, "Preparing the verified 7-Zip console engine...");
            await ArchiveEngineInstall.InstallAsync(cancellationToken);
        }
        else
        {
            using var http = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false })
            {
                Timeout = TimeSpan.FromMinutes(30)
            };
            await new EngineInstaller(http).InstallAsync(package,
                new InlineProgress<EngineInstallProgress>(progress => _messages.Progress(
                    progress.Fraction,
                    $"{progress.Stage}: {progress.ComponentId} ({progress.BytesDone / 1_000_000} MB)")),
                cancellationToken);
        }
        _messages.Result(new ConversionResultMessage(
            true, false, [], Message: $"{package.Id} is ready."));
    }

    private static string ResultNotice(FileResult file, FileFormat source, FileFormat target)
    {
        string note = file.MissingFonts.Count > 0
            ? "Missing fonts were substituted: " + string.Join(", ", file.MissingFonts) : "";
        if (file.NoticeKey is not null)
        {
            note += " " + ErrorText(file.NoticeKey);
        }
        if (source.Id == "pdf" && target.Id == "docx")
        {
            note += " Layout is reconstructed; scanned pages need OCR.";
        }
        return note.Trim();
    }

    private static string ErrorText(string? key)
    {
        if (key is null)
        {
            return "The engine did not produce a completed output.";
        }
        using Stream resource = typeof(ConversionHost).Assembly.GetManifestResourceStream(
            "WeaponCycle.Conversion.en.json")
            ?? throw new InvalidOperationException("Conversion messages are missing.");
        using JsonDocument texts = JsonDocument.Parse(resource);
        return texts.RootElement.TryGetProperty(key, out JsonElement text)
            ? text.GetString() ?? key : key;
    }

    private sealed class InlineProgress<T>(Action<T> report) : IProgress<T>
    {
        public void Report(T value) => report(value);
    }
}
