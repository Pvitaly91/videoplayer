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
