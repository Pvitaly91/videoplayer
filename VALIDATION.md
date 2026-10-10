# Перевірка lifecycle, progress, зуму, зміни розміру відео й compact toolbar

## Фонове кешування прев’ю — 10 жовтня 2026 року

- Release x64 та Win32: по 1305 assertions, failed 0; обидва EXE `--self-test`
  завершилися з кодом 0.
- Підготовка до першого hover, усі часові buckets, синхронні cache hits,
  пріоритет foreground, відкидання late frames після cancel/зміни файла,
  продовження після приховування popup, pause/resume та зміна тривалості
  перевірені на production worker з керованим backend.
- Повторні duration events не перезапускають план. Timeout/Cancelled
  пропускають один bucket; Unsupported/RuntimeFailure/exception зупиняють
  фоновий план без негативного кешування наступного foreground-запиту.
- План містить не більше 121 унікального timestamp, спочатку endpoints,
  потім середини проміжків; перевірено повноту та переповнення до INT64_MAX.
  Production RAM-cache збільшено до 128 кадрів, максимальний кадр 320×180×4.
- Збережено окремі LibVLC player/input та callback-контексти для кожного
  декодування; оптимізація не послаблює захист від pixels попереднього seek.
- Real LibVLC smoke x64 на синтетичному 20-секундному H.264: після ввімкнення
  prefetch усі 5 buckets підготовлені без hover за 2331 мс, перший за 925 мс.
  Наступні 500/500 запитів синхронно отримали кадр із кешу (середнє 1,454 мкс,
  максимум 19 мкс); усі 5 валідних кадрів мають різні fingerprints, exit 0.
  LibVLC друкував converter/imem/teardown warnings; це не warning-free або
  ручна GUI-перевірка. Harness і тестове відео залишені лише в ignored build.

Нижче — попередній звіт від 7 вересня 2026 року. Базовий commit:
`08224a6a33717687efb1d770660925d7cfbb9173`.

Цей документ розділяє причини, які підтверджені кодом або відтворенням,
від захисних виправлень ризиків. Приховані HWND, fake backend, `--self-test`
та інтеграційний запуск LibVLC не називаються ручною GUI-перевіркою.

## Результати за симптомами та новими можливостями

1. **Preview після A → B → A, зуму або помилки.** Підтверджено три дефекти:
   `failedGeneration` блокував generation після одиничного timeout;
   `SetMedia` міг синхронно чекати cleanup worker; перший display callback
   свіжого LibVLC input інколи містив однаковий preroll-buffer для далеких
   timestamp. Окремо доведено, що `get_time` може показувати 0 або відставати
   на секунди від правильного кадру, тому ним не можна кваліфікувати пікселі.
   Preview тепер має окремі Ready, Cancelled, RetryableFailure,
   UnsupportedMedia і RuntimeFailure. SetMedia/Cancel лише інвалідують
   epoch/request та будять один worker. Кожен некешований запит отримує
   свіжий player/input і приватний callback-контекст, який живе до release;
   перший token-matched buffer не публікується, а наступний приймається лише
   з exact generation/epoch/request/attempt. Якщо другого кадру немає,
   лишається placeholder. Retry обмежено двома спробами з 80-мс backoff;
   наступний hover може повторити той самий timestamp. Barrier, retired-
   context callback, outer-worker exception і відновлення мають production
   test seams.

2. **Червона смуга без зуму.** У функції рішення не було залежності від
   zoom, тому конкретну причину видимості лише із crop не доведено.
   Підтверджений ризик — `WS_CHILD` міг опинятися під native video output
   LibVLC. Смуга перенесена в owned, non-activating, mouse-transparent popup
   без global topmost; її координати беруться з фактичного video client rect.
   Автотести перевіряють однакове рішення для zoom None і Applied, ownership,
   відсутність фокуса/Alt+Tab та lifecycle. Візуальний Z-order не перевірено.

3. **Завмирання progress після повторного zoom.** Підтверджено ризик stale
   event generation: старі callback мали mutable identity. Тепер кожен media
   attachment має immutable generation, а slider, label і смуга кожні 100 мс
   використовують один snapshot фактичного main-player із position, duration,
   state і seekable. Seek одразу записує прийняту позицію, а наступний tick
   знову читає backend; час не імітується. У main-only stress один із 45 циклів
   не просунувся за 1,5 с після seek/Play, але сам відновився протягом наступних
   3 с у стані Playing. Це затримка LibVLC, а не доведене постійне зависання.

4. **Прозорість toolbar.** Значення fullscreen overlay змінено з 220 на
   160/255 opacity. High Contrast лишає непрозорий fallback. Click-through
   для інтерактивної панелі не додавався. Читабельність на реальному відео
   візуально не перевірено.

5. **Windowed zoom і кілька незалежних плеєрів.** Підтверджено стару логіку:
   успішний crop викликав EnterFullscreen, а ExitFullscreen скидав crop.
   Обидві прив'язки прибрано. Source crop зберігається через resize,
   minimize/restore та F/F11; Escape виконує рівно одну дію за ієрархією
   Selecting → cancel, Applied → reset, None+fullscreen → exit. Три незалежні
   production-controller fixtures мають різні crop/overlay/progress owners.
   Це перевірка незалежності об'єктів у одному test process, не три видимі
   зовнішні процеси.

6. **Toolbar перестає реагувати після lifecycle-переходів.** Єдину повну
   першопричину відмови всіх кнопок у видимому GUI не відтворено. Натомість
   підтверджено механізм постійного блокування autohide: cancel стирав ознаку
   mouse interaction, але лишав focus на seek/volume/Open; `controlHasFocus`
   після restore безстроково тримав toolbar. Volume slider також міг втратити
   capture без очищення `volumeDragging_`. Capture loss seek помилково
   комітив перемотку, а частковий SetParent міг рознести controls між host.
   Додано one-shot refocus на video лише після mouse interaction (keyboard
   focus зберігається), reconciliation volume capture, одну idempotent cancel
   path, перевірюваний rollback parent і owner gates для minimized/hidden/
   modal станів. Скасування drag не виконує seek; клік і дискретна клавіатурна
   перемотка комітяться один раз. 30 прихованих lifecycle-циклів зберігають
   HWND, controls, crop і snapshots; Open лишається enabled. Реальне клікове
   зависання в GUI не перевірено.

7. **Ручний compact mode у звичайному вікні.** Кнопка
   «Сховати панель керування» і `Ctrl+H`
   примусово ховають панель незалежно від playback state; video child займає
   всю client area, а той самий трьохпіксельний progress popup лишається
   прив'язаним до фактичної області відео. Нижні 48 logical pixels через
   production cursor-to-video conversion показують owned non-activating
   restore popup з alpha 176; поза зоною він ховається, а реальний `BM_CLICK`
   дочірньої owner-drawn кнопки повертає панель. HWND-тести перевіряють точну
   ширину progress, геометрію restore target, відсутність зайвого seek,
   focus, Pause/interaction, minimize/restore, F11, crop/generation та
   ізоляцію двох інстансів. Реальний hover поверх кадру в GUI не перевірено.

8. **Навігація в застосованому зумі.** Plain arrows пересувають crop на 5%
   його розміру; утримання клавіші повторює крок. Лівий drag переміщує
   зображення відносно початкового crop, використовуючи fitted crop rect,
   без накопичення похибки округлення. Колесо змінює масштаб навколо точки
   під курсором; partial wheel delta накопичується. Zoom-out працює одразу
   після selection і доходить до найбільшого прямокутника її пропорцій,
   що вміщується в джерелі, без виходу з Applied mode. Первинний crop
   зберігається окремо від поточного crop, тому pan/wheel його не стирають.
   Навігація лишається доступною по всьому джерельному відео; на максимальному
   віддаленні додаткові оберти колеса не змінюють положення/розмір crop.
   Explicit reset або новий файл очищають початкову selection.
   Crop обмежений джерелом,
   мінімальна сторона zoom-in близько 16 source pixels зі спільним scale.
   `Ctrl+Left/Right` зберігають seek; Z/Escape повертають звичайні стрілки.
   Capture завершується при cancel/loss, resize/move, minimize, змінах
   layout, media і reset. Під час drag fullscreen toolbar не перекриває
   кадр. WM_MOUSEWHEEL перехоплюється перед DispatchMessage для випадку,
   коли Windows спрямувала колесо над відео у сфокусований trackbar.
   Pure/production HWND tests перевіряють межі, anchor, aspect/minimum,
   odd/tiny/invalid inputs, no-drift drag, capture, partial wheel, toolbar/
   letterbox exclusion, transport/generation і незалежні інстанси.

   Маршрут миші звірено з
   [VLC 3.0.23 events.c](https://github.com/videolan/vlc/blob/3.0.23/modules/video_output/win32/events.c#L723-L749):
   PlayerEngine вимикає key і mouse input до play, через що внутрішній
   VLC child має WS_DISABLED. Це дозволяє parent videoWindow приймати
   mouse input без subclass/hook VLC thread.
   [Win32 disabled-child behavior](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#child-windows).
   Це аналіз коду, не виконаний GUI smoke test.

9. **Повтор усього відео та A–B.** Кнопки-прапорці «Повтор відео» і
   «Повтор A–B» взаємовиключні. «Початок A» очищає B й вимикає повтор
   для вільного вибору кінця; валідне B автоматично вмикає фрагмент.
   Мінімальна довжина 500 мс, точки підписані з мілісекундами, діапазон
   підсвічено на реальному trackbar. Додано третій рядок тулбару;
   усі нові controls входять до font, activation, reparent і rollback paths.

   Header-only PlaybackLoop приймає production snapshot і повертає лише
   одноразову дію Seek або Play; Win32/LibVLC дії виконує UI thread.
   Після Seek(A) контролер чекає реальної in-range позиції, а після
   Ended — Playing+seekable перед seek до A. Pending захищає від повторних
   команд на старих snapshots; 5-секундний timeout вимикає цикл.
   PlayerEngine::Restart явно звільняє завершений input через stop перед
   play, навіть якщо запізнілий Stopped уже змінив cached state.
   На реальному portrait-файлі також відтворено перехід A → 0 після
   restart/seek: одиничний in-range snapshot передчасно звільняв pending
   і спричиняв зайвий Seek(A). Для відкладеного seek після restart тепер
   потрібне фактичне просування позиції всередині A–B; повернення нижче A
   скидає кандидат підтвердження. Pure та HWND регресії відтворюють
   послідовність A → 0 → A → A+100/200 без другого seek.
   Manual Stop/Pause скасовують pending; late EndReached після manual Stop
   не має повторно запускати відео. New media/generation та Error скидають
   режим і межі. Зум, fullscreen, compact і незалежні інстанси зберігаються.
   Повтор не є sample/frame-accurate або гарантовано безшовним.

   Спроба GUI-перевірки через computer-use не дійшла до керування вікном:
   kernel завершився з `windows sandbox failed: helper_unknown_error:
   setup refresh had errors`. Живий вигляд нового рядка, читабельність
   підсвічування та візуальна плавність переходу не перевірені.

11. **Попередня пропорційна зміна розміру відображення.** Фіксований
    videoWindow тепер містить окремий videoRenderWindow, до якого прив'язаний
    LibVLC. Рендер-область масштабується рівномірно, а parent кліпує її
    надлишок. Source crop, початкові пропорції selection і presentation
    scale/center зберігаються окремо. Перше захоплення краю застосовує
    full-source crop і переводить UI в Applied; повторне редагування
    не замінює попередній selection.

    Геометрія використовує незмінний anchor жесту, scale від 0.25 до cover
    (із загальною межею 8 і canvas до 16384 px/вісь). Видимі краї кліпінгу
    лишаються доступні, навіть коли всі справжні краї поза viewport.
    Hit-test виключає зовнішню область і видимі controls/preview; resize
    має пріоритет над pan/double-click. Mouse capture і lifecycle
    скасовують обидва жести. Колесо рахує anchor за повним render rect,
    а pan враховує і crop origin, і видимий центр oversized-зображення.
    Це дозволяє рухати навіть full-source crop, який сам уже не має запасу
    для зміни origin. F11, compact і resize зберігають presentation state;
    Z/Escape, новий файл і помилка скидають його.

    Повторна спроба computer-use також завершилась до ініціалізації:
    windows sandbox failed: helper_unknown_error: setup refresh had errors.
    Жести на видимому кадрі, візуальний кліпінг та плавність не перевірені.

12. **Вільні пропорції області зуму (уточнення користувача).** Поточний
    resize редагує вже сам source crop: лівий/правий край змінює ширину
    рамки, верхній/нижній — висоту, кут — обидві незалежно. Пікселі відео
    все ще масштабуються рівномірно; новий crop має пропорції нової рамки.
    Поки є запас джерела, розширення відкриває більше кадру при тій самій
    щільності screen/source pixels. На межі джерела щільність змінюється
    рівномірно для обох осей. Результат обмежено viewport і source bounds.
    Розрахунок від mouse-down не накопичує rounding drift, а нульова зміна
    відновлює точні стартові crop/display/aspect reference. Остання
    відредагована область стає новим aspect reference для колеса; pan
    і F11/compact не повертають пропорції первісного Z-виділення.
    Ця модель замінює aspect-locked поведінку країв із пункту 11.

13. **Менше очікування hover-preview.** Замість фіксованих 750 мс введено
    адаптивний крок 5–30 секунд, округлений до цілих секунд, із ціллю
    близько 120 інтервалів (після години крок обмежено 30 секундами).
    Повторний запит того самого pending/decoding інтервалу повертає його
    request ID і не скасовує capture token. Новий інтервал декодується
    після 100 мс стабілізації наведення; швидкі заміни пересувають deadline,
    а рух усередині інтервалу — ні. Збережено один worker, throttle 120 мс,
    ліміт 24 RAM-кадрів і захист від preroll/stale callback.
    Cache-hit забирається синхронно в поточному hover і відображається без
    проміжного placeholder. Асинхронна публікація, як і раніше, не відкриває
    прихований popup; generation/request/timestamp перевіряються на обох
    шляхах. Час наведення і seek лишаються точними, caption окремо позначає
    приблизний час кадру. Некешований/недоступний кадр все ще може чекати
    на декодер; оптимізація не гарантує миттєвого першого кадру.

14. **Повторне очікування після coarse-preview.** Під час переходу до нового
    bucket UI більше не очищає придатний кадр цього самого файла. Спочатку
    шукає найближчий кешований кадр у межах двох інтервалів, інакше лишає
    попередній. Справжній час зображення не змінюється; pending request має
    позначку оновлення, а bounded failure прибирає її без очищення зображення.
    Якщо fallback немає, після завершеної помилки показується «Кадр
    недоступний» замість активного очікування.
    Fallback не задовольняє новий request ID і не перешкоджає точній заміні.
    Media generation, cancel/hide і заборона асинхронного reopening збережені.
    Додано opt-in adjacent prefetch (увімкнено у MainWindow): до двох
    одноразових спроб тим самим worker у тому самому 24-кадровому RAM-cache,
    без самоповторення чи публікації speculative result у popup. Нове
    наведення/скасування/зміна файла/закриття мають пріоритет. Це зменшує
    кількість холодних запитів, але не гарантує готового кадру для першого
    наведення або довільного стрибка до ще не кешованої позиції.

15. **Експорт відео, A–B і видимого зуму.** Реалізацію виконано від
    заданої бази `9b2ed4cfae5729f2ff2ff952b752531199bdb3e5`.
    Кнопка «Зберегти відео…» і `Ctrl+E` відкривають український
    діалог. Probe стартує лише за цією дією і показує main-video,
    audio, SAR, rational FPS, pixel format/bit depth, rotation, chapters і
    виключені додаткові потоки. До відкриття діалогу фіксуються
    file identity, generation, A/B, source crop, render/presentation і viewport.
    Зміна файла після snapshot зупиняє job.

    Підтримані чотири комбінації whole/A–B × full-frame/visible-zoom,
    source canvas або native crop і оригінальне аудіо/без аудіо. Повний
    кадр без trim/filter є stream copy/remux навіть у «точному» режимі.
    Довільний A–B і будь-який crop перекодовуються; недоведений
    fast A–B stream-copy відхиляється, а не називається точним. Точний
    A–B перекодовує compressed audio в AAC/Opus для коректного timing.
    «Параметри джерела» для crop означають масштабування видимої
    області зі збереженням пропорцій і black padding до coded canvas
    джерела; native crop лишає фактичний вирівняний розмір crop.

    Pure geometry переносить intersection presentation/viewport на source
    crop, тому overlay toolbar і letterbox не входять у результат; pan,
    display scale/clipping, SAR і metadata rotation враховуються без
    подвійного autorotate. Planner перевіряє фактичний encoder inventory і
    вибирає software H.264/HEVC/MPEG-4/VP9/AV1 codec family. Невідомий
    codec переходить у H.264/AAC MP4 лише після окремого підтвердження;
    HDR/10-bit і interlaced downgrade/deinterlace не виконуються непомітно.

    FFmpeg стартує у background thread через `CreateProcessW` без shell/
    console, із приватними pipes й Job Object. Після підтвердження
    плеєр знову інтерактивний; відтворення/preview працюють і можна
    відкрити інший media, не змінюючи job. Закриття плеєра пропонує
    скасувати export і чекає bounded cleanup. Результ пишеться в унікальний
    same-directory `.part`, перевіряється ffprobe і повним decode, потім
    атомарно фіналізується. Same path/hard link, зміна source/destination
    під час job і непідтверджений overwrite блокуються; cancel/error
    видаляє лише власний partial.

    Portable x64 містить FFmpeg/ffprobe 6.1.1 essentials GPL static build
    Gyan.dev, archive SHA-256 `742E32FC9F92681F9F254B925E1B613FDD8074BA40749D4879AEFDB009B94CC5`.
    Win32 містить `n6.0-22-g549430e14d-20230529` GPL static build
    sudo-nautilus/FFmpeg-Builds-Win32, SHA-256
    `CABDE0DF130C32D097099F0F45773A695E2A8F44C6C88E0313757E9B8EB5320B`.
    Обидва archive фіксовані URL/asset ID/size/hash, бінарні файли
    не комітяться; package містить GPLv3 text і provenance.

16. **Видалення кількох відрізків без перекодування.** Позначити A/B
    та натиснути «Додати A–B до списку» кілька разів на H.264/AAC,
    натиснути «Видалити відрізки…» у вікні та fullscreen. Перевірити
    прибирання вибраного відрізка й очищення списку в діалозі. Переконатися, що
    діалог показує початкові й фактичні межі, округлені назовні до
    ключових кадрів, а перекриття після округлення об'єднуються.
    Скасування не змінює джерело та зберігає список. Перевірити збереження
    нового MP4 та перезапис відкритого файла з повторним відкриттям
    плеєра. При зміні джерела/призначення під час копіювання фіналізація
    має бути відхилена. Якщо звільнити відкритий файл не вдалося,
    оригінал має лишитися незмінним, а перевірений partial — доступним.
    Автоматизовані тести перевіряють одиночний A=1,4–B=2,4 с як фактичні
    1–3 с, а також вісім відрізків у зворотному порядку на 18-секундному
    H.264/AAC: дев'ять збережених частин дають 10 с відео й 300 кадрів.
    SHA-256 усіх збережених encoded video packets збігаються з джерелом
    у початковому порядку; encoded audio packets також походять із джерела.
    Перевіряються відсутність накопичення padding на стиках, повне декодування,
    захист оригінала, Unicode/apostrophe paths, cancellation, підготовлений
    partial і атомарна заміна. HWND-тести охоплюють додавання/заборону дублів,
    очищення поточних A/B без втрати списку, ізоляцію вікон і fullscreen.

## Автоматизовані та інтеграційні перевірки

- Вбудована іконка, 10 жовтня 2026: Release x64 і Release Win32 пройшли
  1147/1147 assertions; актуальні EXE `--self-test` — exit 0. Перевірено
  RT_GROUP_ICON і всі дев'ять image resources у test EXE та справжньому
  VideoPlayer.exe, LoadImage для 16/20/24/32/40/48/64/128/256 px, кольорові
  pixels і геометрію alpha/AND-mask, shared large/small HICON у production
  MainWindow class. Native ICO frames 16 і 32 px також переглянуто візуально.
  SHA-256 затвердженого PNG та його project copy збігаються; дизайн не змінено.
- Позначення збережених A–B і заборона перетинів, 30 вересня 2026:
  Release x64 і Release Win32 пройшли 1074/1074 assertions; актуальні EXE
  `--self-test` — exit 0 в обох архітектурах. Pure tests охоплюють точне
  floor/ceil відображення, clipping, subpixel і граничні int64/LONG значення.
  HWND-тести перевіряють відхилення внутрішніх точок/перекриття/охоплення
  через кнопки, клавіші та runtime guards, збереження попередніх A/B і
  незалежність інстансів. Суміжні відрізки дозволені. GDI pixel tests
  перевіряють customdraw channel notifications через обох toolbar parents,
  DPI/resize, компактний `WM_PRINTCLIENT`, color-key gaps, нульовий прогрес
  зі збереженими мітками та очищення. Видимий рендер поверх LibVLC не перевірявся.
- Багатовідрізкове видалення, 30 вересня 2026: Release x64 і Release Win32
  пройшли 1003/1003 assertions; обидва актуальні EXE `--self-test` — exit 0.
  Реальні багатовідрізкові MP4 H.264/AAC, MKV H.264/AAC зі зміщеним початком
  відеопотоку, WebM VP9/Opus і MP4 без аудіо зберегли codec, кількість/порядок
  encoded video packets та їх SHA-256, audio availability і джерело.
  Duration error менше 50 мс; decode-check не виявив помилок. Це не замінює
  інтерактивну перевірку читабельності нового рядка toolbar і діалогу.
- Фінальний export/build/package цикл 13 вересня: 946/946 assertions
  в кожній із Debug Win32, Release Win32, Debug x64 і Release x64.
  Повторні package gates для обох Release також пройшли 946/946,
  portable `--self-test`, PE architecture/dependency audit, exact FFmpeg bytes,
  version/encoder inventory, source no-history для 59 production файлів,
  folder і ZIP privacy/allowlist — зелені. Фінальні ZIP: Win32
  142674109 bytes; x64 141923055 bytes.
- Реальні H.264/AAC CFR/VFR fixture exports в кожній архітектурі:
  whole-full 320×180, 4,000–4,004 с, audio; segment-full 320×180,
  1,9019 с, no audio; whole-crop native 160×90, 3,97063–4,004 с,
  no audio; segment-crop source canvas 320×180, 1,9019 с, audio;
  rotated VFR crop 160×60, 2,96667 с, SAR 4:3, rotation 90°. Кожен
  результат пройшов ffprobe plan validation і повний decode-check.
  Окремо пройшли Unicode >MAX_PATH, доступний local UNC probe,
  corrupt input rejection, cancellation/no-partial, same source/hard-link,
  overwrite і destination/source race protection. Fixture не комітяться.
- Після adjacent prefetch і збереження fallback: 892/892 assertions в усіх
  чотирьох конфігураціях Win32/x64 Debug/Release, без failed assertions.
  Додано 30 перевірок nearest cache (межі, LRU, tie-break, int64 extremes),
  41 lifecycle check prefetch (два сусіди, один worker, no speculative retry,
  збереження foreground result, preemption/cancel/media/disable/shutdown),
  7 synthetic UI checks і 21 assertion з actual injected worker, Win32
  event barriers та production posted notifications. Останні перевіряють
  cold pending → failure → retry → ready, warm fallback/badge, same-bucket
  reuse, точну заміну кадру, failure badge reset і late notification після
  hide, зі збереженням main position та кількості seek-викликів.
- Real LibVLC gate із adjacent prefetch: 70/70 checks, 14 готових кадрів,
  landscape і portrait, 14,4 с. Сусідній інтервал 5 с з’явився в RAM без
  нового наведення за 223/182 мс; наступний foreground-запит до нього
  повернувся за 0 мс. Некешовані кадри у цьому прогоні: 221–341 мс;
  cache-hit: 10–694 мкс. Main pause/crop/seek/playback isolation, cancel,
  audio-only/corrupt recovery і long near-end → beginning збережено.
  Це прихований native harness, а не перевірка видимих жестів/пікселів.
- Після оптимізації coarse preview: 793/793 assertions у кожній із
  чотирьох конфігурацій Win32/x64 Debug/Release. Додано 26 checks: крок
  5–30 с, overflow/endpoints, 121 кадр замість 3201 на sweep 40-хвилинного
  відео, reuse pending/decoding/cache, 20 швидких інтервалів → один decode,
  media generation isolation і негайна презентація лише з поточного hover.
  Перевірки exact seek, cancel/retry/preroll, popup hide, зуму й A–B збережено.
  Один Win32 Debug запуск разом із real-media тестом не дочекався входу
  shutdown fixture у decode за 3 с (792/793); ізольований повтор — 793/793,
  без зміни таймаутів або production-коду.
- Real LibVLC coarse-preview gate 7 вересня: 68/68 checks, 2 цикли,
  14 готових кадрів, 18,1 с на весь процес. Новий некешований інтервал
  на цих файлах займав 177–792 мс, сусідній cache-hit — 2–22 мкс, з
  перевіркою тотожності shared frame і без нового decode. Перевірено
  landscape/portrait, backward і cancel retry, full-source preview під час
  main crop без seek основного paused player, audio-only/corrupt recovery
  та 10-хвилинне відео 580 с → 0 с. Main playback просувався за 266–421 мс.
  Перші два прогони разом з іншими збірками/тестами й потоком LibVLC stderr
  мали deadline failures (63/67 і 61/66); окремо старий portrait probe
  duration/3 потрапив у той самий новий bucket, що й duration/2. Probe
  змінено на окремий 5-секундний bucket. Фінальний запуск ізольовано, stderr
  warnings приглушено; timeout thresholds і декодер не послаблювалися.
  Причину нестабільних затримок попередніх запусків не встановлено.
  Це власні приховані HWND, не інтерактивна GUI-перевірка; швидкість на
  інших кодеках/носіях і при навантаженні не гарантується.
- Після уточнення вільних пропорцій області: 767/767 assertions у кожній
  із чотирьох конфігурацій Win32/x64 Debug/Release. Перевірено незалежні
  осі/кути, розширення source crop, межі джерела, незмінений бік на scale
  floor 0.25, exact no-op/drag-back, новий aspect для колеса, odd/tiny/
  negative geometry та F11/compact/loop/reset. Перший HWND run мав 5
  хибних очікувань: до початку resize child HWND містить також чорні поля,
  тому baseline замінено фактичним fitted content rect; even-source
  округлення допускає до 2 device pixels розбіжності, без спотворення.
- Оновлений Real LibVLC display gate: 65/65, 10,2 с, Release x64 /MT.
  Landscape і portrait додатково проходять Right/Bottom/TopRight зі
  зміною aspect, реальний ApplyVideoCrop, render bounds, просування часу,
  незмінність decoded size/generation та reset/stop/shutdown.
  Перший запуск був 64/65 через 1 device pixel округлення незміненої
  ширини portrait; геометричний допуск виправлено, повторний запуск зелений.
  Неактивне приховане D3D11-вікно видавало nonfatal SetThumbNailClip
  0x800706f4; перевірка не підтверджує візуальну плавність/якість.
- Після додавання presentation resize: 738/738 assertions у кожній із
  чотирьох конфігурацій Win32/x64 Debug/Release. Нові 42 pure checks
  охоплюють pillarbox/portrait/corner resize, cover/min/canvas bounds,
  negative screen coordinates, half-open hit-test, DPI tolerance, видимі
  краї oversized-області та відсутність накопичення похибки жесту.
  Ще 15 production HWND checks перевіряють nested clipping styles,
  double-click пріоритет resize, capture/cancel, пропорційний fill/shrink,
  full-source overscan pan, wheel mapping, незмінність crop/selection,
  F11/compact зі збереженням A–B/generation та reset/new-media/error.
  У першій збірці тестового блоку назва локальної lambda near конфліктувала
  з Windows-макросом; перейменовано на nearlyEqual. Production-виправлення
  за результатами цього блоку не знадобилися.
- Real LibVLC display gate: 47/47 checks, 7,77 с, Release x64 /MT.
  Для landscape 640×360 вкладений render host змінювався до
  1280×720 @ (-320,-180) і 320×180 @ (160,90); для portrait 360×640 —
  до 406×720 @ (117,-180) і 102×180 @ (269,90). Обидва файли створювали
  LibVLC vout descendants, просували час після збільшення/зменшення,
  зберігали crop/generation та коректно виконували reset/stop/shutdown.
  Harness в ignored build/display-real створює власні приховані HWND;
  це перевірка native lifecycle, а не пікселів або живих жестів.
- Після додавання повтору відео/A–B: 681/681 assertions у кожній із
  чотирьох конфігурацій Win32/x64 Debug/Release. Додано 84 pure loop checks
  і 23 production HWND checks. Фактичний Win32 TBM_CLEARSEL повертає
  GETSELSTART/GETSELEND = -1/-1; це перевірено на native control, тести й
  умова повторного очищення узгоджені з цим значенням.
- Real LibVLC loop gate: 73/73 checks, 20,3 с, landscape і portrait.
  На кожному файлі A–B виконав три LoopAction::Seek без Restart; whole-video
  завершення — один Restart; фрагмент до кінця файла — один Restart і
  один відкладений Seek(A), без дублювання при тимчасовій позиції 0.
  Pause/Stop не генерували автоматичних дій; crop і generation збереглися.
  Підготовку harness уточнено стартом усередині фрагмента й pause/seek
  перед перевіркою кінця, щоб setup-seek не зараховувався як повтор.
  Harness у ignored build/loop-real використовує справжній PlayerEngine
  і свіжі Snapshot, але прихований HWND; це не перевірка видимого рендеру.
- Baseline Release x64: 269/269 native tests.
- Baseline перед навігацією в зумі: 492/492 native tests (x64 Release).
- Після відновлення wheel zoom-out: 574 production/regression assertions
  у кожній із чотирьох конфігурацій Win32/x64 Debug/Release. Прибрано помилкову
  заборону розширювати crop понад початковий розмір. Checks охоплюють перше
  outward-колесо після selection, найбільший same-aspect crop, відсутність
  накопичення aspect drift, no-op на межі, повторне наближення, збереження
  pan-позиції й Applied mode, новий вибір/reset та odd/tiny geometry.
- Fake/injected сценарії охоплюють retry/cancel/A→B→A/stale callback,
  slow cleanup, outer worker exit/restart, coherent playback snapshot,
  SetParent rollback, capture/focus cancel, modal/minimize restore, exact
  pointer seek, progress із crop і без нього, ручний compact mode, нижню
  restore-zone, Escape та незалежні controller instances.
- Реальний pan/zoom gate 6 вересня: 54/54 checks, 2 цикли (landscape і
  portrait), по 24 послідовні crop-навігації; LibVLC приймає кожен crop,
  decoded source size лишається незмінним, позиція і стан Paused зберігаються,
  preview лишається full-source. Початковий запуск мав 51/52: одна перевірка
  pause не вклалася у 1500 мс до стартової готовності playback. Harness
  уточнено явним очікуванням Playing перед Pause; сам pause timeout і
  production backend не змінювались. Після цього обидва цикли пройшли,
  playback після seek/Play просунувся за 266–438 мс. Діагностика LibVLC
  у прихованому video-output містила converter/decoder/vout warnings;
  ці тести не доводять якість видимого рендерингу чи плавність миші.
- Реальний LibVLC harness використовує ignored landscape, portrait, audio-only,
  corrupted і 10-хвилинний файл із відмінними кадрами/таймкодом. Файли й
  звіти лишаються в ignored `artifacts` і не входять до commit/package.
- Фінальний real-media gate: 46/46 checks у 2 циклах, 14 готових preview.
  Фінальний довгий прогін: 527/527 checks, 30 циклів, 136 готових preview;
  `SetMedia` максимум 182 мкс, main `Open` максимум 985 мс, shutdown 172 мс.
  Усі 30 main-player liveness checks просунулися за 312–1218 мс при bounded
  threshold 3000 мс. Перевірено A→B→A, forward/backward/cancel/same-time
  preview, crop isolation, main seek/progress, audio-only, corrupted media і
  10-хвилинний файл від 579,750 с назад до 0,750 с. FNV кадрів відповідно
  `4984162070931228799` і `10972959343613852692`; окрема нерівність hashes
  пройшла. Від warmup cycle 5 до cycle 30 threads/GDI/USER лишилися
  38/4/25; private bytes коливалися й змінилися з 152 399 872 до 160 104 448
  (+7 704 576), working set — зі 138 686 464 до 147 836 928. Після teardown:
  threads 13, GDI 0, USER 7, private bytes 24 154 112. Це не доводить
  відсутність повільного росту поза 30 циклами. Harness прихований, не GUI.
- Main-only stress: 45/45 циклів завершено; 44/45 почали просуватися за 1,5 с,
  один цикл відновився сам у додатковому 3-секундному diagnostic window.

## Неперевірене

- інтерактивні GUI-цикли: 0 (desktop automation не стартувала через
  `windows sandbox helper_unknown_error: setup refresh had errors`);
- візуальний progress/restore popup поверх реального video output,
  alpha/читабельність, реальні mouse hover/drag, Open dialog і activation zone;
- інтерактивний pan/zoom на видимому LibVLC, зокрема wheel із фокусом на
  seek/volume та drag через межі вікна;
- три видимі незалежні процеси поруч;
- кілька моніторів, від'ємні координати та різні DPI;
- візуальна відповідність crop на видимому anamorphic/rotated відео
  не перевірена вручну; pure geometry і реальний export metadata/decode gate
  для SAR 4:3 + rotation 90° пройшли;
- Windows 7 SP1, Windows 8.1 і Windows 10 (це лише compatibility targets).

GitHub Actions на `windows-2022` перевіряється окремо після push поточного
commit. CI workflow і portable privacy/dependency checks не послаблювалися.
