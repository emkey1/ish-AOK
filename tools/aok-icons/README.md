# iSH-AOK alternate app icons

Source for the alternate icons in `app/Icons` (the AOK set that replaced the
alternates inherited from upstream iSH).

```
python3 tools/aok-icons/make_svgs.py /tmp/aok-icons/svg
swiftc -O tools/aok-icons/render.swift -o /tmp/aok-icons/render
/tmp/aok-icons/render /tmp/aok-icons/svg /tmp/aok-icons/png    # 1024x1024, opaque
for f in /tmp/aok-icons/png/*.png; do n=$(basename "$f" .png)
  cp "$f" app/Icons/$n.png
  for s in 120 152 167; do sips -Z $s "$f" --out app/Icons/$n-$s.png; done
done
```

The renderer uses WebKit, so fonts come from the Mac that renders them.

`app/Icons/Icons.plist` lists the icons the picker shows. An icon's `aliases`
are retired names (the upstream alternates): iOS remembers the chosen icon by
name, so `tools/generate-ios-alt-icons.py` keeps each alias registered in
`CFBundleAlternateIcons` with the replacement's images, and the picker maps an
alias back to the icon it now shows. Never drop an alias, or anyone still on
that name falls back to the default icon with nothing selected in the picker.

A new icon also needs its three sizes in both `AlternateIconSizes*.xcfilelist`
files and its 1024 image in the app target's resources.
