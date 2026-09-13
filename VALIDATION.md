# Перевірка lifecycle, progress, зуму, зміни розміру відео й compact toolbar

Дата останньої локальної перевірки: 7 вересня 2026 року. Базовий commit:
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

## Автоматизовані та інтеграційні перевірки

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
- anamorphic/non-square pixel aspect ratio та metadata rotation у математиці
  відповідності selection до source crop;
- Windows 7 SP1, Windows 8.1 і Windows 10 (це лише compatibility targets).

GitHub Actions на `windows-2022` перевіряється окремо після push поточного
commit. CI workflow і portable privacy/dependency checks не послаблювалися.
