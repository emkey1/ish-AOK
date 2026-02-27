## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-23 - Table View Selection Accessibility
**Learning:** UITableViewCell selection state (checkmarks) is visual-only by default. VoiceOver users don't know which item is selected unless `UIAccessibilityTraitSelected` is explicitly applied.
**Action:** Always set `accessibilityTraits |= UIAccessibilityTraitSelected` when showing a checkmark accessory, and remove it when hiding the checkmark.
