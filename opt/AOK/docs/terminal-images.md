# Images in the terminal

From build 565 the terminal draws images that programs send with **kitty's
graphics protocol** — the one `kitten icat`, `chafa`, `timg` and a number of
file managers and image viewers use. The image appears where the cursor is,
scrolls with the text, and goes when its lines are cleared.

```sh
sudo apk add kitty-kitten chafa   # Alpine; on Devuan: sudo apt install chafa
kitten icat picture.png            # kitty's own viewer
chafa -f kitty -s 40x12 photo.jpg  # any format chafa reads, 40x12 cells
```

## What is supported

- **Images sent in the escape code itself**, whole or in chunks: PNG, and raw
  RGB or RGBA pixels, compressed with zlib or not.
- **Showing, storing for later, showing a stored image, and deleting** — by
  id, or all at once.
- **Sizing in character cells**, and the cursor either moved past the image or
  left where it started.
- **Answers to the program's questions**, so it can tell images will work.

## Things worth knowing

- **The terminal reports its size in pixels.** `TIOCGWINSZ` now carries the
  text area's width and height in pixels, and the terminal answers `CSI 14 t`,
  `CSI 16 t` and `CSI 18 t`. Without the pixel size, `kitten icat` stopped with
  *Terminal does not support reporting screen sizes in pixels*. Pixels are the
  screen's real pixels, so an image is drawn at its own resolution.
- **Images are sent in the escape code, never by file.** kitty can also pass a
  file path or shared memory; iSH-AOK says no to those, and `icat` and `chafa`
  then send the image directly by themselves. Nothing needs setting.
- **Not supported:** Unicode placeholder mode (`kitten icat
  --unicode-placeholder`, used by some programs inside tmux), animation, cropping
  part of an image, and drawing an image under the text. A program that asks for
  these is told so.
- **A zlib-compressed image needs iOS 16.4 or later.** On older iOS, raw pixels
  sent compressed are refused; PNG still works.
- **Changing the font size afterwards** leaves images already on screen at the
  size they were drawn.
