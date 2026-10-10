# obtan の作業メモ

`C:\projects\windows\CLAUDE.md` の共通方針に加えて、このプロジェクト固有のこと。

## 範囲

**指定のフォルダへダウンロードするところまで**（本人の指示、2026-10-10）。
zip の展開、インストーラーの実行、常駐アプリの停止・再起動は「難しいだろうから」要らないとのこと。

## 確かめ方

- 取得まわりは画面を使わずに確かめられる: `obtan.exe -probe <url> [-dl <dir>] | cat`、`obtan.exe -selftest | cat`。
- 画面は `-ini <検証用 ini>` で起動すると、利用者の `obtan.ini` に触れず、二重起動の制限も外れる。
- 利用者が操作中のことが多い。キーやクリックは送らず、`PostMessage(WM_COMMAND, ID_…)` でボタンを押し、
  `PrintWindow` で窓だけを撮る（前面化しない）。PowerShell 5.1 のスクリプトに日本語のウィンドウ名を書くときは
  UTF-8 BOM 付きで保存する（BOM が無いと ANSI として読まれ、`FindWindow` が外れる）。
  撮るときは `SetProcessDPIAware()` を先に呼ぶ（呼ばないと縮めた座標で撮って右下が切れる）。
- 版を上げるときは `src/obtan.h` の `APP_VERSION` と `src/obtan.rc` の版情報を両方直す。
