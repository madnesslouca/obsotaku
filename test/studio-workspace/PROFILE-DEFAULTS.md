# Portable native profile-default regression

Target `studio-profile-defaults-check` uses one source on Windows and Linux.
It invokes the real frontend profile duplication/switch APIs and reads libobs
configuration values. It starts no outputs, captures no devices, contacts no
network and never opens Settings. This first regression establishes the missing
defaults safely before attempting a Settings constructor on an unfixed build.

The parent controls the process, plugin installation/removal and private config.
Do not run alongside the CUE/ACTIVE probes or load it in a personal session.
Use the [fresh-config preparer](../../tools/prepare-profile-defaults-check.py)
and the standalone `windows-audio` CMake project (Qt Widgets/libobs/frontend API;
no QtTest dependency), or the general workspace test project.

```bash
cmake --build PRIVATE_TEST_BUILD --target studio-profile-defaults-check
```

Required environment variables:

| Name | Meaning |
| --- | --- |
| `OBS_STUDIO_PROFILE_DEFAULTS_ROOT` | New private result directory; existing runtime.json is refused. |
| `OBS_STUDIO_PROFILE_DEFAULTS_CONFIG_ROOT` | Effective configuration root containing `obs-studio`; all configured Locations must resolve here. |
| `OBS_STUDIO_PROFILE_DEFAULTS_EXPECT` | `before` for the preserved unfixed application, `after` for the rebuilt application. Defaults to `after`. |

The initial private profile must be seeded in fresh Simple mode, with **no keys on disk before startup**
for `SimpleOutput/StreamEncoder`, `SimpleOutput/RecEncoder`, `AdvOut/AudioEncoder`
or `AdvOut/RecAudioEncoder`. Normal application startup must supply nonempty
effective/default values for all four. Attach the preparer's pre-start manifest
and file hash as separate runner evidence. This fork's existing
`config_set_item_default` also promotes missing defaults into its in-memory user
sections, so `user_present=true` at startup is expected and does not prove the
seed contained those keys. The probe records that flag without requiring it to
be false. It additionally inspects the current basic.ini without applying defaults
before/after duplication. Do not remove keys in memory or alter the config setter
to manufacture a baseline. This fixture does not silently choose fallback
encoders or change the initial profile's codec settings.

The profile root and four Locations are checked before mutation. The probe
requires an idle application, the frontend thread and closed modal dialogs.
It then:

1. Copies strings for effective, default and user values at startup, including
   distinct null flags. It never holds raw config pointers across frontend calls.
2. Uses native Duplicate Profile and inspects the fresh copy. The fixed result
   must retain all four defaults and valid effective values. Default promotion
   follows this fork's existing semantics. Before the correction, all four
   default slots are absent; effective values are also absent when the copied
   file did not yet contain promoted values. Both facts are recorded separately.
3. Sets valid explicit values only in that owned temporary profile:
   Simple stream/record `x264`, Advanced stream audio `ffmpeg_aac`, recording
   audio `none` (follow stream). It saves and duplicates again.
4. Checks explicit values survive the second duplication while module defaults
   are reapplied beneath them. It switches away/back to verify disk persistence.
5. Restores the initial profile, compares its codec user/default/effective state,
   removes only the two owned temporary profiles and verifies no output started.

`before` is a reproduction mode, not product acceptance. A successful expected
reproduction reports `fixture_completed=true`, `baseline_defect_reproduced=true`
and **`passed=false`**, with the defect explained in `error`. `after` requires
`passed=true`, a completed fixture and restored original profile. Keep both
reports with application/module hashes and external platform provenance.

The first preprobe stopped on an incorrect `user_present=false` assertion before
any duplication. That fixture failure did not establish a product result. Its
replacement accepts the fork's promotion semantics and measures the actual
default/effective state instead; no config setter or production behavior was
changed to accommodate the test.

The product correction reapplies `InitBasicConfigDefaults2` when activating a
runtime profile without resetting outputs. It is gated by an existing output
handler, so initial startup still performs module-aware initialization in its
original post-module phase. Default setters preserve existing user values,
including deliberate selections that differ from the module preference.
The reset=true path is unchanged. Simple encoder defaults are part of this
correction as well as the two Advanced audio defaults.

Each profile operation is marked while on the stack. If a nested event causes
a deadline failure, restoration waits until that call returns. Modal dialogs
must remain closed before cleanup; no generic dialog is accepted or cancelled.
At `SCRIPTING_SHUTDOWN` the fixture prevents late mutations and writes separate
diagnostics without restoring profiles/UI or rewriting the runtime result.

No Settings open/close, output-mode UI change, whole-process profile restart or
native Rename Profile gesture is certified by this module. Rename shares the
same activation path by source inspection but needs its own future interaction
test. The earlier Windows ACTIVE Settings crash remains independently recorded;
removing Settings from this preflight does not classify it as exclusively a
fixture defect or certify the Settings UI. A separate post-fix Settings test must
wait for a real modal with a deadline and forbid restoration while its constructor
or gesture is still on the stack.
