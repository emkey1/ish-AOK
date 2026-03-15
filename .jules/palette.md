## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2024-05-25 - Recycled UITableViewCell State Management
**Learning:** When using reused `UITableViewCell` instances in iOS (e.g., via `dequeueReusableCellWithIdentifier:`), failing to explicitly reset UI state (like `accessoryType`) and accessibility traits (like `UIAccessibilityTraitSelected`) on unselected cells causes them to retain the state of the previously displayed cell. This leads to confusing visual bugs and incorrect VoiceOver announcements.
**Action:** Always explicitly reset `accessoryType` to `UITableViewCellAccessoryNone` and clear the `UIAccessibilityTraitSelected` trait (`cell.accessibilityTraits &= ~UIAccessibilityTraitSelected`) for unselected cells in `cellForRowAtIndexPath:`.
