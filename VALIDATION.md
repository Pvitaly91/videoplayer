# Перевірка lifecycle, progress і windowed zoom

Дата локальної перевірки: 5 вересня 2026 року. Базовий commit:
`7c105d9f5130aca41edfc2cd3a85fbfcaa045231`.

Цей документ розділяє причини, які підтверджені кодом або відтворенням,
від захисних виправлень ризиків. Приховані HWND, fake backend, `--self-test`
та інтеграційний запуск LibVLC не називаються ручною GUI-перевіркою.

## Результати за шістьма симптомами

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
   attachment має immutable generation, а slider, label і смуга кожні 250 мс
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

## Автоматизовані та інтеграційні перевірки

- Baseline Release x64: 269/269 native tests.
- Фінальний набір: 430 production/regression assertions у кожній конфігурації.
- Fake/injected сценарії охоплюють retry/cancel/A→B→A/stale callback,
  slow cleanup, outer worker exit/restart, coherent playback snapshot,
  SetParent rollback, capture/focus cancel, modal/minimize restore, exact
  pointer seek, progress із crop і без нього, Escape та незалежні controller
  instances.
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
- візуальний progress popup поверх реального video output, alpha/читабельність,
  реальні mouse hover/drag, Open dialog і activation zone;
- три видимі незалежні процеси поруч;
- кілька моніторів, від'ємні координати та різні DPI;
- anamorphic/non-square pixel aspect ratio та metadata rotation у математиці
  відповідності selection до source crop;
- Windows 7 SP1, Windows 8.1 і Windows 10 (це лише compatibility targets).

GitHub Actions на `windows-2022` перевіряється окремо після push поточного
commit. CI workflow і portable privacy/dependency checks не послаблювалися.
