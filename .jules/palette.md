## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-23 - Accessibility Labels and Version Checks
**Learning:** Accessibility labels must be assigned unconditionally, outside of any version availability checks (e.g., `@available(iOS 13, *)`) used for newer visual assets like SF Symbols. Otherwise, screen reader support is broken for users on older iOS versions.
**Action:** Always assign `accessibilityLabel` and other accessibility properties outside of version checks.
