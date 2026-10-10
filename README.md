# obtan

アプリの新しい版を見張って、決めたフォルダへダウンロードする Windows アプリ。
Android の [Obtainium](https://github.com/ImranR98/Obtainium) の Windows 版にあたる。

Obtainium と同じように GitHub などのリリースを見張れるほか、**ふつうの Web ページに載っているダウンロードのリンクも見張れる**
（例: <https://zoom-pon.qzy.workers.dev/> の `zoom-pon-v12.zip`）。

できるのは**指定のフォルダへの保存まで**。zip の展開、インストーラーの実行、常駐アプリの停止や再起動はしない。

## 見張れるもの

| 取得元 | URL の例 | 版の決め方 |
|---|---|---|
| GitHub | `https://github.com/notepad-plus-plus/notepad-plus-plus` | リリースのタグ名 |
| GitLab | `https://gitlab.com/グループ/プロジェクト` | リリースのタグ名 |
| Codeberg / Gitea / Forgejo | `https://codeberg.org/所有者/リポジトリ` | リリースのタグ名 |
| Firefox アドオン（AMO） | `https://addons.mozilla.org/ja/firefox/addon/ublock-origin/` | AMO で公開中の版 |
| Web ページ | `https://zoom-pon.qzy.workers.dev/` | リンクのファイル名に含まれる数字 |

取得元は URL から自動で判定する（`github.com`・`gitlab.*`・`codeberg.org`・`addons.mozilla.org` 以外は Web ページ扱い）。登録画面で明示もできる。

### どのファイルを落とすか

「ファイル名の条件」（正規表現）が空のときは自動で選ぶ。

- **リリース**: `.exe .msi .msix .appx .zip .7z .xpi` のうち、Windows x64 向けらしいもの
  （`win` `windows` `x64` `amd64` などを含むものを優先し、`linux` `mac` `arm64` などは避ける。
  `.exe` > `.msi` > `.zip` > `.xpi` の順）。Firefox 拡張を GitHub のリリースで配っているもの（`.xpi`）もこれで選ばれる。最新のリリースに該当が無ければ、ひとつ前のリリースを見る。
  プレリリースは既定では対象外。
- **Firefox アドオン**: AMO が配っている `.xpi`。
- **Web ページ**: `href` / `src` と本文中の URL のうち、`.exe .msi .msix .appx .zip .7z .xpi` で終わるもの。
  複数あれば版がいちばん新しいもの。

条件を書くときは正規表現で、大文字小文字は区別しない。リリースではファイル名に、Web ページでは URL 全体に当てる。
例: `x64.*\.zip$`、`portable\.x64\.zip$`。

### 版の決め方

「版の取り出し」（正規表現）が空のときは自動。

- リリースはタグ名そのもの（`v8.9.8.1` など）。
- Web ページはファイル名から版らしい数字を拾う（`zoom-pon-v12.zip` → `12`、`7z2501-x64.exe` → `2501`）。
  `x64` `win32` `64bit` の数字は拾わない。
- ファイル名に版が無い（`setup.exe` のように名前が変わらない）ときは、そのファイルの `Last-Modified`
  （無ければ `ETag`）を版の代わりにする。

正規表現を書くときは、`( )` で囲んだ部分が版になる。例: `-v(\d+)\.zip`。

新しいかどうかは、版の中の最初の「数字.数字…」を数として比べる（`1.10` は `1.9` より新しい）。
数字が同じで後ろだけ違う（`1.2.0-beta1` → `1.2.0`）ときと、数字を含まない版が変わったときも「新しい版」とみなす。

## 使い方

1. `obtan.exe` を好きな場所に置いて起動する。
2. 「追加...」で URL を入れ、「調べる」で最新の版と落とすファイルを確かめてから OK。
   名前が空なら、リポジトリ名やページのタイトルが入る。
3. 一覧の右クリックで「ダウンロード」。新しい版があるものは「新しい版をすべて取得」でまとめて落とせる。

- 保存先は、登録ごとの「保存先フォルダ」。空なら `既定の保存先\名前\`（既定の保存先は「設定」で変える。
  初期値は `ダウンロード\obtan`）。`%USERPROFILE%` のような環境変数も使える。
- 同じ名前のファイルがあれば上書きする。古い版のファイルは消さない。
- ダウンロードすると「取得済みの版」がその版になる。手で直すこともできる（右クリックの「取得済みにする」「未取得に戻す」も）。
- 「確認の間隔」（既定 6 時間）ごとに全件を確認する。新しい版は通知し、
  「自動でダウンロードする」にした登録はそのまま保存する。
- 閉じるボタンではトレイにしまう（設定で変えられる）。終了はトレイの右クリックから。
- 「Windows の起動時にトレイで起動する」は `HKCU\...\Run` に登録する。
- 画面は Windows のライト／ダークの設定に追従する。

キー操作: Enter で編集、Delete で削除、F5 ですべて確認、Ctrl+A で全選択。

### 注意

- GitHub の API は、トークン無しだと 1 時間に 60 回まで。登録が多いときは「設定」で GitHub のトークンを入れる
  （公開リポジトリだけなら権限は要らない）。トークンは `obtan.ini` に平文で保存される。
- Web ページは HTML をそのまま読む。JavaScript で後からリンクを作るページでは見つからない。
- 保存したファイルには「インターネットから入手した」印（Mark of the Web）を付けない。
- `.xpi` は保存するだけ。Firefox に入れるときは、保存した `.xpi` を Firefox の窓へドラッグする。

## 設定ファイル

`obtan.exe` の隣の `obtan.ini`（UTF-16）。そこに書けないとき（Program Files など）は `%APPDATA%\obtan\obtan.ini`。
登録した一覧、確認の結果、設定、ウィンドウの位置が入る。

## コマンドライン

```
obtan.exe                 一覧を開く
obtan.exe -tray           トレイにしまった状態で起動する
obtan.exe -ini <path>     別の ini で起動する（二重起動の制限も外れる）

obtan.exe -probe <url> [-kind github|gitlab|gitea|web|amo] [-filter 正規表現] [-ver 正規表現] [-pre] [-token t] [-dl フォルダ]
                          最新の版と落とすファイルを標準出力に出す。-dl を付けるとそこへ保存する
obtan.exe -selftest       正規表現・版の比較・ファイル選びの自己テスト
```

`-probe` と `-selftest` は GUI の exe なので、PowerShell からは `| Out-String`、Git Bash からは `| cat` のように
パイプでつなぐと終わりまで待って出力を受け取れる。

## ビルド

```
build.cmd
```

Visual Studio Build Tools 2022（C / MSVC）を vswhere で探して `cl` でビルドする。CRT は静的リンク。

| ファイル | 役割 |
|---|---|
| `src/obtan.c` | 画面、トレイ、定期確認、ダウンロードの順番待ち、コマンドライン |
| `src/source.c` | 取得元ごとの最新版の判定、ファイル選び、版の比較 |
| `src/net.c` | WinHTTP での取得とダウンロード |
| `src/json.c` | API の応答を読む最小限の JSON パーサー |
| `src/rx.c` | 小さな正規表現（バックトラック方式） |
| `src/util.c` | 文字列とバッファの小道具 |
