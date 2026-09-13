# Third-party notices

Цей файл описує сторонні компоненти portable-пакета. Він не встановлює
ліцензію для власного коду репозиторію.

## VideoLAN VLC / LibVLC 3.0.23

- Проєкт: VideoLAN VLC media player та LibVLC.
- Версія: 3.0.23.
- Офіційне джерело:
  <https://download.videolan.org/pub/videolan/vlc/3.0.23/>.
- Вихідний код релізу:
  <https://download.videolan.org/pub/videolan/vlc/3.0.23/vlc-3.0.23.tar.xz>.
- Основний VLC поширюється за умовами GNU General Public License,
  version 2 or later (GPL-2.0-or-later).
- Бібліотеки LibVLC поширюються за умовами GNU Lesser General Public
  License, version 2.1 or later (LGPL-2.1-or-later).
- Окремі бібліотеки, модулі та плагіни можуть мати власні сумісні ліцензійні
  умови; авторитетними є тексти та notices, що входять до офіційного архіву
  VideoLAN.

До Win32 і x64 portable-пакетів із відповідного офіційного архіву входять:

- `libvlc.dll`;
- `libvlccore.dll`;
- кореневі runtime DLL, потрібні LibVLC і плагінам;
- каталог `plugins` без довільного видалення декодерів;
- `plugins.dat`, один раз згенерований під час отримання runtime офіційною
  утилітою `vlc-cache-gen.exe` з того самого binary archive; сама утиліта до
  portable-пакета не входить;
- `COPYING.LIB` — незмінений текст LGPL-2.1 з official source tag 3.0.23;
- інші офіційні runtime-каталоги (наприклад, Lua/localization/assets), якщо
  вони є в архіві;
- доступні офіційні файли `COPYING*`, `LICENSE*`, `AUTHORS*`,
  `THANKS*`, `NEWS*` і `README*`.

Development SDK (`sdk/include`, `sdk/lib`), `vlc.exe`, деінсталятор та
debug-файли до portable-пакета не включаються.

Текст GPL і copyright/author notices постачаються з official binary archive;
відсутній у Windows ZIP текст LGPL береться без змін з `COPYING.LIB` official
VideoLAN source tag 3.0.23 і також входить до portable-папки. Якщо цей файл і
вкладені офіційні тексти відрізняються, застосовуються офіційні тексти
відповідного компонента.

## FFmpeg / ffprobe

Exporter запускає окремі static executables `tools/export/ffmpeg.exe` і
`tools/export/ffprobe.exe`; LibVLC залишається playback backend. Бінарні
архіви не зберігаються у Git. `scripts/fetch-ffmpeg.ps1` завантажує їх лише
з release assets, перевіряє фіксований SHA-256 і staging додає до portable
package офіційний GPLv3 text та `FFMPEG_BUILD.txt` із provenance.

- x64: FFmpeg 6.1.1 essentials static build від Gyan.dev,
  <https://github.com/GyanD/codexffmpeg/releases/tag/6.1.1>;
  GitHub asset `143137017`, 87520045 bytes, provider commit
  `6473e72373c86cbf4173cda6b2f44827ed5cce2e`;
  archive SHA-256
  `742E32FC9F92681F9F254B925E1B613FDD8074BA40749D4879AEFDB009B94CC5`.
- Win32/x86: FFmpeg `n6.0-22-g549430e14d-20230529` GPL static build від
  sudo-nautilus/FFmpeg-Builds-Win32,
  <https://github.com/sudo-nautilus/FFmpeg-Builds-Win32>;
  GitHub asset `110307584`, 95961101 bytes, provider tag commit
  `f7d241136493035122299d3e91e06a7ca8168ddc`;
  archive SHA-256
  `CABDE0DF130C32D097099F0F45773A695E2A8F44C6C88E0313757E9B8EB5320B`.
- FFmpeg source releases: <https://ffmpeg.org/releases/>.
- FFmpeg legal/licensing overview: <https://ffmpeg.org/legal.html>.

Обидва вибрані binary builds є GPLv3 і містять software H.264/AAC encoders,
потрібні для явно підтвердженого fallback та frame-accurate exports. Повний
текст `LICENSE.GPLv3.txt`, copyright/configuration від самих tools і посилання
на відповідні sources постачаються поруч із executables. Деякі включені
libraries мають додаткові сумісні умови; авторитетними є bundled license і
build configuration конкретного archive.
