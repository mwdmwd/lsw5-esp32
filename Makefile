PROJECT := lsw5-esp32.kicad_pro
PCB := $(PROJECT:.kicad_pro=.kicad_pcb)
RENDER_OUT := img/lsw5-esp32.png
SHADOW_ALPHA := 0

.PHONY: render
render: $(RENDER_OUT)

$(RENDER_OUT): $(PCB) $(PROJECT) tools/kicad-viewport.py Makefile
	kicad-cli pcb render \
		--output "$@" \
		--width 2900 \
		--height 3800 \
		--background transparent \
		--quality high \
		--preset follow_pcb_editor \
		--perspective \
		--zoom 0.7 \
		--light-top 0 \
		--light-bottom 0 \
		--light-side 0.5 \
		--light-camera 0 \
		$(shell tools/kicad-viewport.py $(PROJECT) readme) \
		--pan '0,0,0' \
		"$<"

	magick "$@" \
		-alpha set \
		-channel A -fx 'a < 0.98 ? a * $(SHADOW_ALPHA) : a' +channel \
		-trim +repage \
		-bordercolor none -border 50 \
		\( +clone -background black -shadow 80x12+0+0 \) \
		+swap -background none -layers merge +repage \
		"$@"

	pngquant --force --speed 1 --quality 80-90 --output "$@" "$@"
