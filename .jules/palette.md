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

## 2024-06-03 - Dynamic Accessibility Labels for State-Changing Buttons
**Learning:** In iOS UI development, when a single button dynamically changes its visual icon and primary function based on state (e.g., setting a new title or action dynamically), its `accessibilityLabel` must be dynamically updated alongside the visual change to accurately describe the current actionable state to VoiceOver users.
**Action:** When updating a button's visual representation (e.g., via `setTitle:`), ensure you also dynamically update `accessibilityLabel` to match the new state or function.

## 2024-06-04 - Explicit Labels for Implicitly Grouped UI Controls
**Learning:** In iOS UI development, interactive input elements like `UITextField` that lack adjacent visible textual labels (such as a browser address bar) must have an explicitly set `accessibilityLabel` to ensure VoiceOver users can clearly identify their purpose, even if placeholder text exists. Additionally, ensuring all controls in a toolbar (like 'Home' and 'Go' buttons) have `accessibilityLabel`s ensures a consistent experience for screen reader users.
**Action:** When adding or modifying unlabeled input fields or icon/text buttons in toolbars, always set a clear `accessibilityLabel` property.
