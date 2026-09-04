namespace VideoPlayer.Helpers;

public static class PlaybackMath
{
    public static long ClampPosition(long positionMilliseconds, long durationMilliseconds)
    {
        if (durationMilliseconds <= 0)
        {
            return 0;
        }

        return Math.Clamp(positionMilliseconds, 0, durationMilliseconds);
    }

    public static long CalculateSeekTarget(
        long currentPositionMilliseconds,
        int offsetSeconds,
        long durationMilliseconds)
    {
        if (durationMilliseconds <= 0)
        {
            return 0;
        }

        var currentPosition = ClampPosition(currentPositionMilliseconds, durationMilliseconds);
        var offsetMilliseconds = offsetSeconds * 1_000L;

        if (offsetMilliseconds >= 0)
        {
            var remaining = durationMilliseconds - currentPosition;
            return offsetMilliseconds >= remaining
                ? durationMilliseconds
                : currentPosition + offsetMilliseconds;
        }

        return offsetMilliseconds <= -currentPosition
            ? 0
            : currentPosition + offsetMilliseconds;
    }

    public static int ClampVolume(int volume) => Math.Clamp(volume, 0, 100);
}
