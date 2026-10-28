using Filee.Core.Conversion;
using WeaponCycle.Conversion;

internal static class CatalogChecks
{
    public static void Run()
    {
        for (var availability = 0; availability < 32; availability++)
        {
            var converters = new[]
            {
                new Fake("direct", (availability & 1) != 0, [new("a", "b", 5)]),
                new Fake("bridge", (availability & 2) != 0, [new("a", "c", 5), new("c", "b", 5)]),
                new Fake("text", (availability & 4) != 0, [new("b", "d", 5, Unshaped: true)]),
                new Fake("shaped", (availability & 8) != 0, [new("b", "d", 15)]),
                new Fake("unused", (availability & 16) != 0, [new("z", "z")])
            };
            var expected = new ConverterCatalog(converters).CreatePlanner();
            foreach (var from in new[] { "a", "b", "c", "d", "z" })
            {
                foreach (var to in new[] { "a", "b", "c", "d", "z" })
                {
                    var chosen = ConversionCatalog.ForConversion(converters, from, to, packArchive: false);
                    var actual = new ConverterCatalog(chosen).CreatePlanner();
                    foreach (var shaping in new[] { false, true })
                    {
                        var fullRoute = expected.Plan(from, to, shaping: shaping);
                        var selectedRoute = actual.Plan(from, to, shaping: shaping);
                        if (fullRoute?.Cost != selectedRoute?.Cost)
                        {
                            throw new InvalidDataException($"Route changed: {from}/{to}/{shaping}/{availability}");
                        }
                    }
                }
            }
        }
        var relevant = new Fake("relevant", true, [new("a", "b")]);
        var unrelated = new Fake("unrelated", true, [new("x", "y")]);
        ConversionCatalog.ForConversion([relevant, unrelated], "a", "b", false);
        if (relevant.Probes != 1 || unrelated.Probes != 0)
        {
            throw new InvalidDataException("Conversion probed unrelated engines.");
        }

        Console.WriteLine("PASS 1,600 route/availability/shaping comparisons and unrelated-engine probe guard.");
    }

    private sealed class Fake(string id, bool available, IReadOnlyList<ConversionEdge> edges) : IConverter
    {
        public string Id => id;
        public string DisplayName => id;
        public int MaxParallelism => 1;
        public IReadOnlyList<ConversionEdge> Edges => edges;
        public int Probes
        {
            get; private set;
        }
        public EngineStatus GetStatus()
        {
            Probes++;
            return available ? EngineStatus.Available() : EngineStatus.Unavailable("not installed");
        }
        public Task<IReadOnlyList<string>> ConvertAsync(
            ConversionStep step, IProgress<double>? progress, CancellationToken cancellationToken)
            => throw new NotSupportedException("The catalog check never converts files.");
    }
}
