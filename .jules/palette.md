## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility labels hidden by OS version checks
**Learning:** Accessibility labels should not be gated by OS version checks used for visual assets like SF Symbols, as this breaks screen readers on older OS versions.
**Action:** Always assign `accessibilityLabel` and other a11y properties outside `@available()` checks.
