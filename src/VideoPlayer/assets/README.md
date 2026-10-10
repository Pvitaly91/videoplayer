# Application icon

`VideoPlayer.png` is the approved original artwork, created with the built-in
imagegen tool on 10 October 2026. It is copied into the repository unchanged;
the original transparent alpha is preserved. No CLI/API fallback was used.

`VideoPlayer.ico` is a format conversion of that artwork, not a redesign. Run
`scripts/convert-app-icon.ps1` from the repository to recreate it with Windows
System.Drawing. It contains 16, 20, 24, 32, 40, 48, 64, 128 and 256 px images.
Small frames use 32-bit DIBs with an AND mask; the 256 px frame is a PNG.
The ICO is embedded in `VideoPlayer.rc` as `IDI_VIDEOPLAYER`, so the application
does not need an external icon file at runtime.

## Original imagegen prompt

Use case: logo-brand. Asset type: one original Windows desktop video-player app
icon concept, square composition. Primary request: an elegant instantly readable
icon for a video player with region zoom and A–B video cutting. One dark graphite
rounded-square tile, a large solid warm-white right-pointing Play triangle centered
within a bold orange cropped-video frame made of four separate L-shaped corner
brackets. Four identical thick corner brackets with clean consistent strokes;
balanced negative space between frame and triangle. The Play triangle is the
strongest visual element and should read at 16px and 32px. Palette inspired by the
app's saved A–B orange markers: charcoal #18212B, warm white #F6F7F8, vivid orange
#FF9900. Flat geometric graphic with immaculate sharp edges and subtly rounded
joins, straight-on 2D; original clean professional software branding, not a photo
or product mockup. Single icon only, centered, tile occupies about90% of square
canvas; genuinely transparent pixels outside rounded-square silhouette. No text,
letters, numbers, watermark, film perforations, scissors, timeline, glow, shadows
outside the silhouette, ornamental lines, gradients, glass effects, 3D perspective,
comparison grids or backgrounds.
