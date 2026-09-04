using VideoPlayer.Helpers;

namespace VideoPlayer.Tests;

public sealed class CommandLineFileResolverTests
{
    [Fact]
    public void ResolveFirstExistingFile_PreservesSpacesAndUnicodeInPath()
    {
        var path = Path.Combine(
            Path.GetTempPath(),
            $"VideoPlayer Tests {Guid.NewGuid():N} — відео файл.mp4");

        try
        {
            File.WriteAllBytes(path, []);

            var resolved = CommandLineFileResolver.ResolveFirstExistingFile([$"\"{path}\""]);

            Assert.Equal(Path.GetFullPath(path), resolved);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void IsExistingLocalFile_ReturnsTrueForExistingFile()
    {
        var path = Path.GetTempFileName();

        try
        {
            Assert.True(CommandLineFileResolver.IsExistingLocalFile(path));
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void ResolveFirstExistingFile_ReturnsNullForMissingFile()
    {
        var path = Path.Combine(Path.GetTempPath(), $"{Guid.NewGuid():N}.mp4");

        Assert.Null(CommandLineFileResolver.ResolveFirstExistingFile([path]));
    }

    [Fact]
    public void ResolveFirstExistingFile_ReturnsNullForEmptyArguments()
    {
        Assert.Null(CommandLineFileResolver.ResolveFirstExistingFile([]));
        Assert.Null(CommandLineFileResolver.ResolveFirstExistingFile(null));
    }

    [Fact]
    public void ResolveFirstExistingFile_ReturnsNullForDirectory()
    {
        Assert.Null(CommandLineFileResolver.ResolveFirstExistingFile([Path.GetTempPath()]));
    }

    [Fact]
    public void ResolveFirstExistingFile_OnlyConsidersFirstArgument()
    {
        var path = Path.GetTempFileName();

        try
        {
            Assert.Null(CommandLineFileResolver.ResolveFirstExistingFile(["missing.mp4", path]));
        }
        finally
        {
            File.Delete(path);
        }
    }
}
