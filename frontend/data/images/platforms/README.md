# Platform artwork

One SVG per platform, named after the platform id in `StreamPlatform.cpp`
(`twitch.svg`, `youtube.svg`, `kick.svg`, `facebook.svg`, `tiktok.svg`,
`x.svg`, `trovo.svg`).

`PlatformIconProvider` loads the file, tints it with the brand color from the
platform catalog and renders it into the channel cards and the add-channel
grid. A platform with no file here falls back to its initial drawn over the
same brand color, so a missing file is never a broken interface.

## Where these came from

The files shipped here are from [Simple Icons](https://simpleicons.org), whose
icon collection is released under **CC0 1.0 Universal** (public domain
dedication). See `../../license/simple-icons.txt`.

CC0 covers the collection, not the brands: each logo remains the trademark of
its owner. The use here is nominative — identifying which service a destination
sends to — which the platform brand guidelines allow. Anyone redistributing
this application should still check the current guidelines of each platform,
because they change:

- Twitch: <https://brand.twitch.tv>
- YouTube: <https://www.youtube.com/howyoutubeworks/resources/brand-resources/>
- Kick: <https://kick.com/brand>
- Meta / Facebook: <https://about.meta.com/brand/resources/>
- TikTok: <https://www.tiktok.com/about/brand-guidelines>
- X: <https://about.x.com/en/who-we-are/brand-toolkit>
- Trovo: <https://trovo.live>

Trovo has no Simple Icons entry, so it uses the drawn fallback.

## Requirements for a replacement file

- Single-color SVG with no `fill` on the root element: the provider injects the
  brand color there and the paths inherit it.
- Square viewBox, ideally `0 0 24 24`.
- No embedded raster images or external references.
