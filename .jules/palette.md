## 2024-05-23 - Accessibility for Toggle Buttons
**Learning:** Avoid using custom "1"/"0" strings in `accessibilityValue` for toggle states.
**Action:** Use `UIAccessibilityTraitSelected` dynamically in `setSelected:`. This provides standard, localized voice feedback (e.g., "Selected") automatically.
