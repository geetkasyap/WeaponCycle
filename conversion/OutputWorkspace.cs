namespace WeaponCycle.Conversion;

internal sealed class OutputWorkspace : IDisposable
{
    private const int MaximumOutputNames = 9999;
    private readonly string _parentDirectory;
    private readonly string _outputName;
    private bool _published;
    private bool _disposed;

    public OutputWorkspace(string sourcePath)
    {
        _parentDirectory = Path.GetDirectoryName(Path.GetFullPath(sourcePath))
            ?? throw new ArgumentException("The source must have a parent directory.", nameof(sourcePath));
        _outputName = Path.GetFileNameWithoutExtension(sourcePath) + " (converted)";
        DirectoryPath = Path.Combine(
            _parentDirectory, ".weaponcycle-" + Guid.NewGuid().ToString("N") + ".partial");
        Directory.CreateDirectory(DirectoryPath);
    }

    public string DirectoryPath
    {
        get;
    }

    public string[] Publish(IReadOnlyList<string> outputs, CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_published || outputs.Count == 0)
        {
            throw new InvalidOperationException("Publish one complete, non-empty result set.");
        }
        string[] relativePaths = outputs.Select(Path.GetFullPath)
            .Select(path => Path.GetRelativePath(DirectoryPath, path)).ToArray();
        if (relativePaths.Any(path => Path.IsPathRooted(path) || path == ".." ||
            path.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal)))
        {
            throw new InvalidDataException("A conversion result was outside its private output workspace.");
        }
        if (outputs.Any(path => !File.Exists(path) && !Directory.Exists(path)))
        {
            throw new InvalidDataException("A conversion result was missing before publication.");
        }
        for (int number = 1; number <= MaximumOutputNames; number++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            string name = number == 1 ? _outputName : $"{_outputName} ({number})";
            string destination = Path.Combine(_parentDirectory, name);
            string[] publishedPaths = relativePaths.Select(path => Path.Combine(destination, path)).ToArray();
            try
            {
                Directory.Move(DirectoryPath, destination);
                _published = true;
                return publishedPaths;
            }
            catch (IOException) when (Directory.Exists(destination) || File.Exists(destination))
            {
                if (number == MaximumOutputNames)
                {
                    throw;
                }
            }
        }
        throw new IOException("No unused result folder name was available.");
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        if (!_published && Directory.Exists(DirectoryPath))
        {
            Directory.Delete(DirectoryPath, recursive: true);
        }
    }
}
