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
## 2026-04-03 - RootsTableViewController Accessibility Improvements
**Learning:** Static `UITableViewCell` instances used as action buttons in iOS do not automatically receive `UIAccessibilityTraitButton`. Furthermore, when cells are reused for stateful data (like identifying a default filesystem), simply adding `UIAccessibilityTraitSelected` leads to state leaks across reused cells unless the trait is explicitly cleared for non-selected items using `&= ~UIAccessibilityTraitSelected`.
**Action:** When implementing custom static action cells, always manually assign `UIAccessibilityTraitButton` (e.g., in `tableView:willDisplayCell:forRowAtIndexPath:`). When managing traits based on dynamic state in reusable cells, ensure all states are explicitly handled (both adding and clearing traits) to prevent VoiceOver from reading stale information.
