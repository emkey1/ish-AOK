## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2024-05-25 - UITableViewCell Selection Accessibility and Reuse
**Learning:** When managing state for reused `UITableViewCell` instances in iOS, relying only on `accessoryType` for visual state is insufficient for screen readers, and failing to clear state on unselected cells causes UI bugs when cells are recycled. VoiceOver users may not be aware of selection states indicated by checkmarks if traits are not updated.
**Action:** Always explicitly reset `accessoryType` to `UITableViewCellAccessoryNone` and clear the `UIAccessibilityTraitSelected` trait (`cell.accessibilityTraits &= ~UIAccessibilityTraitSelected`) for unselected cells, while setting the selected trait when `UITableViewCellAccessoryCheckmark` is used.

## 2024-05-26 - Destructive Updates to accessibilityTraits
**Learning:** Overwriting `accessibilityTraits` using the assignment operator (`=`) when toggling states (e.g., `selected ? UIAccessibilityTraitSelected : 0`) deletes all inherent traits, such as `UIAccessibilityTraitButton`, causing VoiceOver to no longer announce the control type correctly.
**Action:** Always use bitwise OR (`|=`) to add traits and bitwise AND NOT (`&= ~`) to remove traits to preserve existing state.

## 2024-05-27 - Overriding accessibilityTraits Getter Destructively
**Learning:** When creating custom accessible controls by subclassing existing ones, completely overriding the `accessibilityTraits` getter (e.g., `return UIAccessibilityTraitAdjustable;`) destructively removes inherent superclass traits (such as `UIAccessibilityTraitButton`).
**Action:** Always use a bitwise OR with `[super accessibilityTraits]` (e.g., `return [super accessibilityTraits] | UIAccessibilityTraitAdjustable;`) when overriding the `accessibilityTraits` getter to preserve necessary inherited traits.

## 2024-05-28 - Exposing Visual Badges to Screen Readers
**Learning:** Visual indicators, such as a red badge signaling an available update, are invisible to screen readers unless their state is programmatically exposed.
**Action:** When adding or updating visual badges on UI elements, always set a corresponding descriptive `accessibilityValue` (e.g., `@"Update available"`) on the element or its parent container when the badge is visible, and clear it (`nil`) when the badge is hidden, avoiding cluttering the main `accessibilityLabel`.

## 2024-05-29 - Workspace Scene Button Selection Accessibility
**Learning:** Workspace scene buttons and browser tab buttons in the app's workspace dynamically change their visual state (background and border color) based on selection but failed to update their `accessibilityTraits`. This leaves VoiceOver users unaware of the active scene or tab.
**Action:** Always dynamically apply `UIAccessibilityTraitSelected` (`|=`) to active scene/tab buttons and clear it (`&= ~`) for inactive ones when updating their visual styles.
