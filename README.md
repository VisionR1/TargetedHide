# TargetedHide (THIDE)


[![GitHub release (latest by date)](https://img.shields.io/github/v/release/VisionR1/TargetedHide?label=Release&labelColor=black&color=purple&style=for-the-badge)](https://github.com/VisionR1/TargetedHide/releases/latest)
[![GitHub Release Date](https://img.shields.io/github/release-date/VisionR1/TargetedHide?label=Release%20Date&labelColor=black&color=purple&style=for-the-badge)](https://github.com/VisionR1/TargetedHide/releases)
[![GitHub Releases](https://img.shields.io/github/downloads/VisionR1/TargetedHide/latest/total?label=Downloads%20%28Latest%20Release%29&labelColor=black&color=orange&style=for-the-badge)](https://github.com/VisionR1/TargetedHide/releases/latest)
[![GitHub All Releases](https://img.shields.io/github/downloads/VisionR1/TargetedHide/total?label=Total%20Downloads%20%28All%20Releases%29&labelColor=black&color=orange&style=for-the-badge)](https://github.com/VisionR1/TargetedHide/releases)

[![TargetedHide Official XDA Thread](https://img.shields.io/badge/XDA_Forums-TargetedHide_Official_Thread-0055AA?logo=XDA-Developers&logoColor=white&labelColor=black&style=for-the-badge)](https://xdaforums.com/t/module-targetedhide-thide.4801278/)

## Purpose

A Zygisk module that hides specific files, folders, or apps from specific other apps — and only those apps. Everything else on your device sees your files completely normally.

For example: you can prevent a specific app from seeing a folder on your storage, while every other app (your file manager, gallery, etc.) still sees that folder perfectly fine.

---

## What it actually does

- **Hides files and folders** from chosen apps. The hidden app doesn't just see an empty folder — it genuinely can't find it at all, the same as if it never existed.
- **Hides installed apps** from other apps' "installed apps" lists ([see below](#under-the-hood-hiding-from-installed-apps-lists)).
- Only affects the specific apps you choose. Nothing is hidden system-wide, and nothing is touched, moved, or deleted on your storage — ever. (Yes, that includes `/system` — [see below](#can-i-hide-files-in-system-or-other-core-os-paths) for what that actually means.)

---

## Requirements

To use this module you need one of the following combinations:

- [Magisk](https://github.com/topjohnwu/Magisk) with Zygisk enabled or provided by a module, and either DenyList enabled or disabled if using [Shamiko](https://github.com/LSPosed/LSPosed.github.io/releases) or [Zygisk Assistant](https://github.com/snake-4/Zygisk-Assistant) or [NoHello](https://github.com/MhmRdd/NoHello) module.
- [KernelSU](https://github.com/tiann/KernelSU) with [Zygisk Next](https://github.com/LSPosed/ZygiskNext) or [ReZygisk](https://github.com/PerformanC/ReZygisk) or [NeoZygisk](https://github.com/JingMatrix/NeoZygisk) module.
- [KernelSU Next](https://github.com/KernelSU-Next/KernelSU-Next) with [Zygisk Next](https://github.com/LSPosed/ZygiskNext) or [ReZygisk](https://github.com/PerformanC/ReZygisk) or [NeoZygisk](https://github.com/JingMatrix/NeoZygisk) module.
- [APatch](https://github.com/bmax121/APatch) with [Zygisk Next](https://github.com/LSPosed/ZygiskNext) or [ReZygisk](https://github.com/PerformanC/ReZygisk) or [NeoZygisk](https://github.com/JingMatrix/NeoZygisk) module.

> [!NOTE]
> Android 8.0 (API 26) or newer.

---

## Installation

1. Download the latest from [Releases](../../releases)
2. Flash it in Magisk / KernelSU / APatch, like any other module
3. Reboot
4. Edit the [config files](#configuration), then just force-stop and reopen the app you're hiding from — **no reboot needed** for config changes

---

## Configuration

All config lives in `/data/adb/modules/targetedhide/config/`. You can edit these files with any root file manager or text editor.

### `target.txt` — which apps get anything hidden from them

One app per line. This is the list of apps that TargetedHide is even active in — anything not listed here sees your device completely unmodified.

```
com.example.somecleaner
com.example.somefilemanager
```

If an app has multiple processes (shown as `package:something` in a process list), you can write either `package:something` or `package.something` — both work the same.

### `path.txt` — what to hide, for every app in `target.txt`

One path per line. Applies to **every** app listed in `target.txt`, unless that app has its own [Per-app file](#per-app-files--different-rules-for-different-apps).

```
/storage/emulated/0/SomeFolder
/storage/emulated/0/somefile.txt
```

> [!NOTE]
> If you add a file, folder, or package to your config that doesn't actually exist on your device yet, nothing bad happens. The module simply ignores that line and moves on — no crashes, no errors.

### Per-app files — different rules for different apps

Want one specific app to have its own, different hide list instead of the shared `path.txt`? Create a file named with the package name of the app, e.g.:

```
com.example.somefilemanager.txt
```

If this file exists (even if empty), **it completely replaces `path.txt` for that one app** — `path.txt` is ignored for it. Every other app in `target.txt` still uses `path.txt` as normal.

### Hiding an app by name, without knowing its exact folder

Since Android randomizes app install folders (like `/data/app/~~AbCd==/com.example-EfGh==`), writing the exact path is impractical. Instead, just write the app's **package name**, with no `/` in front:

```
com.example.someapp
```

This hides that app's folder wherever it actually is, and also hides it from installed-app lists.

> [!IMPORTANT]
> If you use HMA-OSS or other app-hiding tools on the same target app, [see here](#does-this-replace-hma-oss) before adding package names to your config.

### Excluding one thing from a broader rule

Start a line with `!` to make it an **exception** — this always wins over any hide rule, no matter what order the lines are in.

```
/data/app
!com.example.appyouwanttokeepvisible
```

This hides everything under `/data/app`, except that one app.

> [!IMPORTANT]
> If you hide something broad like `/data/app`, you must exclude every app listed in `target.txt` itself, or each of them will hide their own files from themselves and crash on launch. See the [full example](#full-example) below for exactly what this looks like.

This also works for plain files and folders, at any depth — not just app names. If you exclude something nested a few folders deep inside a hidden folder, every folder along the way down to it stays browsable too, so you can actually reach it:

```
/storage/emulated/0/Photos
!/storage/emulated/0/Photos/Vacation/2026/example.jpg
```

Browsing into `Photos` here shows nothing except the path leading to `example.jpg` — every other file and folder inside `Photos` stays hidden as normal.

### Full example
<details>
  <summary>Click to view complete config examples</summary>

`path.txt`:
```
# Standard folder and file hiding
/storage/emulated/0/Download/Example
/storage/emulated/0/DCIM/example.jpg

# Keep specific files or subfolders visible inside a hidden path
!/storage/emulated/0/Download/Example/Allowed.txt

# Package matching (hides specific app install folders)
com.example.someapp

# If hiding the entire /data/app directory:
/data/app

# IMPORTANT: You MUST exclude every target app using this global list!
# Otherwise, they will hide their own files and crash on launch.
!com.example.firsttarget
!com.example.secondtarget

# You can also exclude any other apps you want this target app to still see
!com.example.appyouwanttokeepvisible
```

`com.example.somefilemanager.txt`:
```
# Standard folder and file hiding
/storage/emulated/0/Download/Example
/storage/emulated/0/DCIM/example.jpg

# Keep specific files or subfolders visible inside a hidden path
!/storage/emulated/0/Download/Example/Allowed.txt

# Package matching (hides specific app install folders)
com.example.someapp

# If hiding the entire /data/app directory:
/data/app

# IMPORTANT: You MUST exclude the target app itself! 
# Otherwise, it will hide its own files and crash on launch.
!com.example.somefilemanager

# You can also exclude any other apps you want this target app to still see
!com.example.appyouwanttokeepvisible
```
</details>

---

## Under the hood: Hiding from "installed apps" lists

TargetedHide relies on native C++ hooks to hide standard files and folders. However, making an app completely disappear from other apps' "installed apps" screens requires intercepting Android's Java-based PackageManager APIs. 

To achieve this, the module seamlessly injects a small piece of compiled Java code (`classes.dex`) directly into the target app's process alongside the native hooks.

- **If you are using a pre-built release zip:** This Java injection is already included and runs automatically. You don't need to configure anything extra.
- **If you are building from source:** The provided Gradle build script automatically compiles and packages this `classes.dex` file for you.

---

## Can I hide files in /system or other core OS paths?

Mechanically, yes — the hooks don't treat `/system` any differently than any other path. But it's worth understanding why this is a genuinely different situation than hiding a personal folder or an app, before you do it.

**There's rarely a real reason to.** `/system` is read-only, identical across every device running the same build, and contains no personal data — it's shared OS infrastructure, not something private to hide. The main use cases this module is built for (hiding your own files, hiding a specific installed app) don't apply here the same way.

**The real risk: breaking the app you're hiding it from, not your phone.** If a target app happens to legitimately read something under `/system` as part of its normal operation (a shared library, a system font, a resource it loads at startup) and you hide that specific path from it, that app can crash or misbehave — the exact same category of risk as the [`/data/app` self-exclusion warning](#excluding-one-thing-from-a-broader-rule) earlier in this README, just for a different kind of path.

**What this can *never* do: affect your whole system, or anything outside the app you configured.** Two things guarantee that:
- Every hook only ever runs inside one specific target app's own process — never system-wide, never in any process you haven't explicitly listed in `target.txt`.
- `system_server` — Android's core OS process — is permanently and unconditionally excluded. This module never loads into it at all, regardless of anything in your config. There's no rule you can write that changes this.

So worst case, if you hide something under `/system` that a target app actually needed: that one app breaks, until you fix your config. Nothing else on your device is ever at risk.

---

## Known limitation — please read this before reporting it as a bug

### Root file managers using shell commands to list files

> [!WARNING]
> Some root file managers (and similar tools) don't read folders directly — instead, they run a command like `su -c ls /some/folder` behind the scenes to list files faster. That command runs completely outside of what this module (or really any Zygisk module) can reach.

In that specific case:
- The **file/folder name** may still be visible in that one listing view
- But actually opening it, reading it, or getting real details about it is still fully blocked

This isn't a bug we can fix from here — it's a fundamental limit of how Zygisk works, not something specific to this module. Direct access is what actually matters for privacy/security, and that remains solid.

### Apps that refuse to open when targeted

> [!WARNING]
> Some apps actively scan their own memory to check whether they've been modified — a common anti-tamper protection in virtual-environment apps, app cloners, and other heavily-shielded software. Since TargetedHide works by injecting hooks directly into a target app's memory (via Zygisk and Dobby), an app with this kind of protection will detect the injection and shut itself down as a defense measure — refusing to launch, freezing on a loading screen, or crashing instantly the moment it's added to `target.txt`.

This isn't a bug in the module — it's the target app successfully defending itself against memory modification. If an app is specifically built to detect and block hooking frameworks, it genuinely cannot be targeted by this module, no matter how the config is set up.

---

### Does this replace HMA-OSS?

**No.** This module's main purpose is hiding files and folders — PackageManager hiding here is a small bonus feature, not the project's focus. [HMA-OSS](https://github.com/frknkrc44/HMA-OSS) is a dedicated, considerably more advanced project built specifically for comprehensive app-hiding across far more detection methods than TargetedHide covers. If you need robust app-hiding on its own, use HMA-OSS.

> [!WARNING]  
> Do not use the TargetedHide package-hiding feature on an app that is already targeted by HMA-OSS (or other app-hiding tools). Combining their app-hiding features on the same app will conflict and likely cause it to crash.

---

## Disclaimer

> [!CAUTION]
> This module modifies low-level filesystem behavior via Zygisk. I am not responsible for bricked devices, bootloops, lost data, app bans, or any other damage resulting from the use of this module. You are choosing to make these modifications to your device at your own risk.

---

## Credits

* **[topjohnwu/zygisk-module-sample](https://github.com/topjohnwu/zygisk-module-sample):** Core module implementation framework.
* **[JingMatrix/Dobby](https://github.com/JingMatrix/Dobby):** Native hooking library.
* **[LSPosed/AndroidHiddenApiBypass](https://github.com/LSPosed/AndroidHiddenApiBypass):** Java hidden API bypass for injection.
* **[frknkrc44/HMA-OSS](https://github.com/frknkrc44/HMA-OSS):** Conceptual inspiration for the PackageManager-hiding approach.
* **[VisionR1/TargetedFix](https://github.com/VisionR1/TargetedFix):** Built following the same targeted philosophy.

---

## License

TargetedHide is licensed under the GPLv3 — see [LICENSE](LICENSE)
