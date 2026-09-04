using System.IO;

namespace VideoPlayer.Helpers;

public static class CommandLineFileResolver
{
    public static string? ResolveFirstExistingFile(IReadOnlyList<string>? arguments)
    {
        if (arguments is null || arguments.Count == 0)
        {
            return null;
        }

        return ResolveExistingFile(arguments[0]);
    }

    public static string? ResolveExistingFile(string? argument)
    {
        if (string.IsNullOrWhiteSpace(argument))
        {
            return null;
        }

        var path = RemoveMatchingQuotes(argument.Trim());

        try
        {
            if (!Path.IsPathFullyQualified(path))
            {
                return null;
            }

            var fullPath = Path.GetFullPath(path);
            return File.Exists(fullPath) ? fullPath : null;
        }
        catch (Exception exception) when (exception is ArgumentException
                                              or NotSupportedException
                                              or PathTooLongException
                                              or UnauthorizedAccessException)
        {
            return null;
        }
    }

    public static bool IsExistingLocalFile(string? path) => ResolveExistingFile(path) is not null;

    private static string RemoveMatchingQuotes(string path)
    {
        return path.Length >= 2 && path[0] == '"' && path[^1] == '"'
            ? path[1..^1]
            : path;
    }
}
