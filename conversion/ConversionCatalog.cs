using Filee.Core.Conversion;

namespace WeaponCycle.Conversion;

internal static class ConversionCatalog
{
    public static IReadOnlyList<IConverter> ForConversion(
        IReadOnlyList<IConverter> converters, string from, string to, bool packArchive)
    {
        if (packArchive)
        {
            return converters.Where(c => c is IFileCombiner).ToArray();
        }

        var candidates = converters.ToList();
        var available = new Dictionary<string, bool>();
        var selected = new HashSet<string>();

        Route? Resolve(bool shaping)
        {
            while (new RoutePlanner(candidates).Plan(from, to, shaping: shaping) is { } route)
            {
                var unavailable = new List<IConverter>();
                foreach (var converter in route.Steps.Select(s => s.Converter).DistinctBy(c => c.Id))
                {
                    if (!available.TryGetValue(converter.Id, out var ready))
                    {
                        ready = converter.GetStatus().IsAvailable;
                        available[converter.Id] = ready;
                    }
                    if (!ready)
                    {
                        unavailable.Add(converter);
                    }
                }
                if (unavailable.Count == 0)
                {
                    foreach (var step in route.Steps)
                    {
                        selected.Add(step.Converter.Id);
                    }

                    return route;
                }
                foreach (var converter in unavailable)
                {
                    candidates.Remove(converter);
                }
            }
            return null;
        }

        var primary = Resolve(shaping: false);
        // JobQueue may replan complex text; retain the available shaped alternative.
        if (primary?.Steps.Any(s => s.Unshaped) == true)
        {
            Resolve(shaping: true);
        }

        return converters.Where(c => selected.Contains(c.Id)).ToArray();
    }
}
