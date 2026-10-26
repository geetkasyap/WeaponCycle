namespace WeaponCycle.Conversion;

internal enum ConversionCommand
{
    Inspect,
    Convert,
    Install
}

internal sealed record ConversionRequest(
    ConversionCommand Command, string Input, string? TargetFormat = null)
{
    internal static bool AcceptsInputLength(long length) =>
        length is > 0 and <= 2L * 1024 * 1024 * 1024;

    public static ConversionRequest Parse(string[] arguments)
    {
        ConversionRequest request = arguments switch
        {
            ["inspect", var source] => new(ConversionCommand.Inspect, source),
            ["convert", var source, var target] => new(ConversionCommand.Convert, source, target),
            ["install", var package] => new(ConversionCommand.Install, package),
            _ => throw new ArgumentException(
                "Use inspect <file>, convert <file> <format>, or install <engine>.")
        };
        if (string.IsNullOrWhiteSpace(request.Input) ||
            (request.Command == ConversionCommand.Convert &&
             string.IsNullOrWhiteSpace(request.TargetFormat)))
        {
            throw new ArgumentException("Supply a file or engine and a supported output format.");
        }
        return request;
    }
}
