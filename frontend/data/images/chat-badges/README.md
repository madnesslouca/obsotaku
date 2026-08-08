# Chat role badges

One file per role, named after the short label `BuildRoleBadges()` produces in
`MultiStreamChatAggregator.cpp`, lowercased:

| File | Role | Shown for |
| --- | --- | --- |
| `host.png` | Broadcaster | the channel owner |
| `mod.png` | Moderator | moderators |
| `vip.svg` | VIP | VIPs |
| `sub.svg` | Subscriber | subscribers and members |

`ChatBadgeIcons` looks for `<role>.png` first and `<role>.svg` second, so a
hand-made badge can replace a drawn one by dropping the file in, with no code
change. A role with no file here keeps the lettered pill it had before, so a
missing file is never a broken interface.

Unlike the platform logos in `../platforms`, these arrive already coloured and
nothing is tinted at render time. A replacement should therefore carry its own
background: a square with rounded corners, a white glyph inside, ideally a
`0 0 36 36` viewBox or a 36×36 bitmap.

## Where these came from

`host.png` and `mod.png` are Twitch's own chat badges, supplied by the user of
this fork. `vip.svg` and `sub.svg` are drawn here to match them, because no
equivalent file was available.

The Twitch badges remain Twitch's marks. The use here is nominative — showing
that a chatter holds that role on that platform, in a client that displays
Twitch chat — which the brand guidelines allow. Anyone redistributing this
application should check the current guidelines, because they change:
<https://brand.twitch.tv>.

Note also that these badges only make sense next to Twitch messages. The
aggregator applies them to every platform, so a Kick moderator gets Twitch's
sword. Replacing them with per-platform artwork, or with the badge URLs the
Twitch API returns for the channel, would be the honest fix.
