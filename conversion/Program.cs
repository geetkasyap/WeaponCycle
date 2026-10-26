using System.Diagnostics;
using System.Text;
using Filee.Engines.Infrastructure;
using ImageMagick;

namespace WeaponCycle.Conversion;

internal static class Program
{
    public static async Task<int> Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        Console.InputEncoding = new UTF8Encoding(false);
        var messages = new ConversionMessages(Console.Out);
        using var cancellation = new CancellationTokenSource();
        using var stopListener = new CancellationTokenSource();
        Task listener = ListenForCancellationAsync(
            Task.Run(Console.ReadLine), cancellation, stopListener.Token);
        try
        {
            string dataDirectory = ConfigureEnvironment();
            ConversionRequest request = ConversionRequest.Parse(args);
            return await new ConversionHost(messages, dataDirectory)
                .RunAsync(request, cancellation.Token);
        }
        catch (OperationCanceledException)
        {
            messages.Result(new ConversionResultMessage(
                false, true, [], Message: "Cancelled. Original kept."));
            return 1;
        }
        catch (Exception error)
        {
            // This process boundary reports failures; implementation methods do not swallow them.
            Console.Error.WriteLine(error);
            messages.Result(new ConversionResultMessage(false, false, [], Error: error.Message));
            return 1;
        }
        finally
        {
            await stopListener.CancelAsync();
            await listener;
        }
    }

    private static string ConfigureEnvironment()
    {
        string dataDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "OrbitDeck", "conversion");
        EngineEnvironment.DownloadRoot = Path.Combine(dataDirectory, "engines");
        EngineEnvironment.SearchSystem = false;
        EngineEnvironment.LibreOfficeForDocuments = true;
        EngineEnvironment.LibreOfficeForSheets = true;
        using Process process = Process.GetCurrentProcess();
        process.PriorityClass = ProcessPriorityClass.BelowNormal;
        ResourceLimits.Thread = 2;
        ResourceLimits.Memory = 256UL * 1024 * 1024;
        ResourceLimits.Disk = 2UL * 1024 * 1024 * 1024;
        ResourceLimits.Area = 64_000_000;
        return dataDirectory;
    }

    private static async Task ListenForCancellationAsync(
        Task<string?> input, CancellationTokenSource cancellation, CancellationToken stopToken)
    {
        try
        {
            string? command = await input.WaitAsync(stopToken);
            if (command is null or "cancel")
            {
                await cancellation.CancelAsync();
            }
        }
        catch (OperationCanceledException) when (stopToken.IsCancellationRequested)
        {
            // Stop awaiting redirected stdin before the request's cancellation source is disposed.
        }
        catch (IOException error)
        {
            Console.Error.WriteLine(error);
            await cancellation.CancelAsync();
        }
    }
}
