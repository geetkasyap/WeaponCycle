using System.Text.Json;

namespace WeaponCycle.Conversion;

internal sealed record ConversionTargetMessage(
    string Id, string Label, bool Available, string[] Packages, long DownloadBytes);

internal sealed record ConversionFormatsMessage(
    string SourceFormat, string Category, IReadOnlyList<ConversionTargetMessage> Targets)
{
    public string Type => "formats";
}

internal sealed record ConversionResultMessage(
    bool Succeeded, bool Cancelled, string[] Outputs, string Error = "", string Message = "")
{
    public string Type => "result";
}

internal sealed class ConversionMessages(TextWriter output)
{
    private static readonly JsonSerializerOptions s_jsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };
    private readonly TextWriter _output = output;
    private readonly Lock _writeLock = new();

    public void Progress(double fraction, string message) =>
        Write(new
        {
            Type = "progress",
            Progress = fraction,
            Message = message
        });

    public void Formats(ConversionFormatsMessage formats) => Write(formats);

    public void Result(ConversionResultMessage result) => Write(result);

    private void Write<T>(T message)
    {
        lock (_writeLock)
        {
            _output.WriteLine(JsonSerializer.Serialize(message, s_jsonOptions));
            _output.Flush();
        }
    }
}
