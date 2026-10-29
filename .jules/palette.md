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

## 2024-06-05 - Accessibility Labels for Factory-Generated Buttons
**Learning:** When using a factory method to generate icon-only buttons with fallback text strings (like "M", "|<", ">|"), setting `accessibilityLabel` equal to the abbreviated fallback text does not provide enough context for VoiceOver users.
**Action:** In button generation methods, expand abbreviated text placeholders into clear, descriptive `accessibilityLabel` values (e.g., mapping "M" to "Menu", ">|" to "Next Track") so VoiceOver users understand the button's purpose without relying on visual context.
## 2026-09-01 - Added accessibilityHint to Modern Menu Pip
**Learning:** VoiceOver screen reader users need additional context on what the "Workspace menu" button does. A label alone doesn't clarify its function.
**Action:** Added an `accessibilityHint` explaining that the button opens the desktop menu for window management.

## 2024-05-29 - Missing Accessibility Hints on App Buttons
**Learning:** Adding an `accessibilityLabel` to a UI element allows a screen reader to announce its name, but if its purpose or effect isn't clear, VoiceOver users won't know what action will result from activating it.
**Action:** Always provide an `accessibilityHint` that concisely describes the outcome of activating the element (e.g., "Navigates back to the previous page", "Closes the current workspace window").

## 2024-05-30 - Missing Accessibility Hints in Applet Toolbars
**Learning:** Adding an `accessibilityLabel` to a UI element allows a screen reader to announce its name, but if its purpose or effect isn't clear, VoiceOver users won't know what action will result from activating it. This applies to applet toolbars where actions might be context-specific.
**Action:** Always provide an `accessibilityHint` that concisely describes the outcome of activating the element for applet toolbars (e.g., "Navigates back to the previous document.", "Cancels the video extraction process.").
## 2024-09-26 - Add VoiceOver hints to file manager navigation buttons
**Learning:** Icon-only navigation buttons in toolbars (like Back, Forward, Up, More Actions) often only have short `accessibilityLabel`s which lack context for screen readers. In iOS, these buttons can benefit greatly from an `accessibilityHint` to provide clearer action descriptions.
**Action:** When evaluating toolbars with icon-only buttons, ensure an `accessibilityHint` is provided alongside the `accessibilityLabel` if the label alone is not sufficiently descriptive of the resulting action (e.g., "Navigates to the previously visited folder." instead of just "Back").
## 2024-05-31 - Missing Accessibility Hints in Standard Toolbars
**Learning:** Adding an `accessibilityLabel` or having an implicit label on a text button allows a screen reader to announce its name, but if its outcome isn't totally obvious, VoiceOver users won't know what action will result from activating it. This applies to simple text-based toolbar buttons like "Reset" or "Duplicate".
**Action:** Always provide an `accessibilityHint` that concisely describes the outcome of activating the element for applet toolbars (e.g., "Creates a copy of the current theme for editing.").

## 2024-05-14 - Add accessibilityHint to New Tab button
**Learning:** Icon-only buttons with an `accessibilityLabel` benefit from an `accessibilityHint` to clarify the action. Screen reader users hear the label ("New Tab") and then the hint ("Opens a new terminal tab.") to explain the outcome. This is especially important for buttons that have complex actions or lack visual text.
**Action:** When adding or verifying accessibility labels for icon-only buttons, especially those using system icons, always ensure there is an accompanying `accessibilityHint` to provide full context on what the button will do.
## 2026-10-16 - Contextual Accessibility Hints for Vague Terminology
**Learning:** Toolbar buttons using terse or ambiguous labels like "Model" or "Chats" may not clearly convey their function to screen reader users, even if visually apparent from the context.
**Action:** Always provide an `accessibilityHint` to clarify the outcome of activating the button when its label uses vague terminology (e.g., "Shows a list of all configured models." for a "Model" button).

## 2026-10-17 - Add accessibility hints and labels to Workspace applets
**Learning:** In iOS UI development, `UIButton` instances configured with a symbol using `barButtonWithTitle` or similar methods may not have their titles reliably announced by VoiceOver, depending on the view hierarchy or system version. Additionally, symbols used for layout management (like `arrow.clockwise` for restore or `arrow.down.doc` for session) are not always self-explanatory to visually impaired users.
**Action:** When adding icon buttons or buttons with a complex combination of title and symbol, always explicitly set the `accessibilityLabel`. Furthermore, for actions where the visual icon doesn't completely explain the result (like workspace layout management), always provide an `accessibilityHint` that concisely describes the outcome (e.g., "Saves the current workspace layout.").
## 2026-10-18 - Missing Accessibility Labels for System Bar Button Items
**Learning:** `UIBarButtonItem` instances initialized using `initWithBarButtonSystemItem:` (like `UIBarButtonSystemItemAction` or `UIBarButtonSystemItemRefresh`) do not always inherently provide a descriptive `accessibilityLabel` that matches their visual icon (e.g., Share or Refresh), relying instead on generic system behavior which can sometimes fall back to the view's properties or just announce "button".
**Action:** When adding or verifying system-styled bar button items, explicitly assign an `accessibilityLabel` to ensure clear and consistent VoiceOver announcements, alongside the `accessibilityHint`.
## 2026-10-22 - Missing Accessibility Labels and Hints for System Bar Button Items in Snippets
**Learning:** `UIBarButtonItem` instances initialized using `initWithBarButtonSystemItem:` (like `UIBarButtonSystemItemAdd`, `UIBarButtonSystemItemDone`, `UIBarButtonSystemItemCancel`, `UIBarButtonSystemItemSave`) do not always inherently provide a descriptive `accessibilityLabel` or `accessibilityHint` that accurately conveys their function to VoiceOver users, relying instead on generic system behavior.
**Action:** When adding or verifying system-styled bar button items, explicitly assign an `accessibilityLabel` (especially for icon-only buttons like Add) and an `accessibilityHint` to ensure clear and consistent VoiceOver announcements.
## 2026-10-24 - Accessibility for UIBarButtonSystemItem

**Learning:** `UIBarButtonItem` instances initialized via `initWithBarButtonSystemItem:` (like UIBarButtonSystemItemDone or UIBarButtonSystemItemAdd) automatically receive natively localized titles by Apple. However, VoiceOver may not announce *what* action the button performs in context, which can be ambiguous (e.g. "Done", but done with what?).
**Action:** Always provide an explicit `accessibilityHint` that concisely describes the outcome of activating the element (e.g. "Dismisses the file browser") to provide VoiceOver users with clear context. For symbol-only items like Add or Compose, it may also be necessary to explicitly assign an `accessibilityLabel` (e.g. "New Chat" instead of relying on the system default).
## 2026-10-25 - Add accessibility hint to file change revert button
**Learning:** When using `initWithTitle:` to create a `UIBarButtonItem`, if the title itself does not clearly communicate the action's outcome (especially if the state changes dynamically, like 'Revert' vs 'Reverted'), an explicit `accessibilityHint` is critical for VoiceOver users to understand what the button will do.
**Action:** Always provide an explicit `accessibilityHint` for context-specific buttons created with `initWithTitle:` where the resulting action isn't fully conveyed by the label alone.

## 2024-11-20 - Hide breadcrumb chevrons from VoiceOver
**Learning:** Decorative text separators like chevrons (›) between breadcrumb buttons are read aloud by VoiceOver, causing unnecessary clutter during navigation.
**Action:** Always set `accessibilityElementsHidden = YES` on purely decorative separators in accessibility structures.
