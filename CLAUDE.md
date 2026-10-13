# obtan の作業メモ

`C:\projects\windows\CLAUDE.md` の共通方針に加えて、このプロジェクト固有のこと。

## 範囲

**指定のフォルダへダウンロードするところまで**（本人の指示、2026-10-10）。
zip の展開、インストーラーの実行、常駐アプリの停止・再起動は「難しいだろうから」要らないとのこと。

**常駐しない・自動確認しない**（本人の指示、2026-10-11）。トレイ、定期確認、通知、起動時の自動起動、
登録ごとの自動ダウンロードは v6 で外した。回数制限が解けたあとのやり直しだけは、開いている間に限り残している。

## 確かめ方

- 取得まわりは画面を使わずに確かめられる: `obtan.exe -probe <url> [-dl <dir>] | cat`、`obtan.exe -selftest | cat`。
- 画面は `-ini <検証用 ini>` で起動すると、利用者の `obtan.ini` に触れず、二重起動の制限も外れる。
- 利用者が操作中のことが多い。キーやクリックは送らず、`PostMessage(WM_COMMAND, ID_…)` でボタンを押し、
  `PrintWindow` で窓だけを撮る（前面化しない）。PowerShell 5.1 のスクリプトに日本語のウィンドウ名を書くときは
  UTF-8 BOM 付きで保存する（BOM が無いと ANSI として読まれ、`FindWindow` が外れる）。
  撮るときは `SetProcessDPIAware()` を先に呼ぶ（呼ばないと縮めた座標で撮って右下が切れる）。
- 一覧のチェックボックスを外から切り替えるときは、一覧コントロールに `WM_LBUTTONDOWN` / `WM_LBUTTONUP` を
  チェックボックスの位置（x=12、y=見出しの高さ＋行の高さ×行番号＋行の高さ/2）へ `PostMessage` する。
  行を選ぶのはラベル部分（x=80 など）へのクリック、Ctrl 付きは wParam に `MK_CONTROL`。状態は
  `LVM_GETITEMSTATE` で読める。`LVM_SETITEMSTATE` を他プロセスのメモリに LVITEM を書いて送る方法は、
  状態が 0 になってチェックボックスが消えた（原因は追っていない）。PowerShell 5.1 の UI オートメーションは
  Win32 コントロールが Pane にしか見えず使えなかった。
- 回数制限は GitHub の `/rate_limit` で確かめられる（これは回数に数えられない）。
- リリースノートと README の取得は `obtan.exe -notes <url> [-tag t] | cat`、`obtan.exe -readme <url> | cat` で確かめられる。

## GitHub のノートと README の取り方（2026-10-13 に実測）

- `/releases.atom`・リポジトリのトップ・`/releases/expanded_assets/<タグ>` は API の残り回数を減らさない（56 → 56）。
- Atom に載るのは最新 10 件（プレリリースも含む）。`<content type="html">` は実体参照で包んだ HTML。
- `/releases/tag/<タグ>` の画面は本文を React の埋め込みデータで持ち、`markdown-body` が HTML に無い。古いタグは API（1 回）に頼る。
- リポジトリのトップは README を `<article class="markdown-body entry-content container-lg">` に描いている（入れ子の article は無い）。
- `expanded_assets` の `<relative-time datetime>` は添付の登録日時。最大値は API の `published_at` と数秒の差（v31: 11:37:24 と 11:37:26）。
- AMO の `release_notes` は版ごとの `/versions/<版>/` にあり、書かれていない版は null（uBlock Origin 1.75.0）。
- 版を上げるときは `src/obtan.h` の `APP_VERSION` と `src/obtan.rc` の版情報を両方直す。
