# kiln documentation site
#
# Builds a small static site from the project docs:
#   index.html   from ../README.md
#   manual.html  from ../doc/kiln.1 (rendered man page)
#
# Usage:
#   make          build the site into _site/
#   make serve    build, then serve at http://127.0.0.1:8000/
#   make check    verify output has no project-Pages-breaking paths
#   make clean    remove _site/
#
# Requires pandoc. OUT is overridable for CI (e.g. make OUT=public).

OUT    ?= _site
PANDOC ?= pandoc
ROOT   := ..

TEMPLATE := template.html
COMMON   := --standalone --template=$(TEMPLATE)

.PHONY: all site serve check clean

all: site

site: $(OUT)/index.html $(OUT)/manual.html $(OUT)/style.css \
      $(OUT)/screenshot.svg $(OUT)/.nojekyll

$(OUT):
	mkdir -p $(OUT)

$(OUT)/index.html: $(ROOT)/README.md $(TEMPLATE) | $(OUT)
	sed 's|doc/screenshot.svg|screenshot.svg|g' $(ROOT)/README.md \
	  | $(PANDOC) $(COMMON) --from gfm --metadata title="kiln" -o $@

$(OUT)/manual.html: $(ROOT)/doc/kiln.1 $(TEMPLATE) | $(OUT)
	$(PANDOC) $(COMMON) --from man \
	  --metadata title="kiln(1) manual" -o $@ $(ROOT)/doc/kiln.1

$(OUT)/style.css: style.css | $(OUT)
	cp $< $@

$(OUT)/screenshot.svg: $(ROOT)/doc/screenshot.svg | $(OUT)
	cp $< $@

$(OUT)/.nojekyll: | $(OUT)
	touch $@

serve: site
	@echo "Serving $(OUT) at http://127.0.0.1:8000/ (Ctrl-C to stop)"
	@cd $(OUT) && python3 -m http.server 8000

check: site
	@fail=0; \
	for f in index.html manual.html style.css screenshot.svg .nojekyll; do \
	  [ -e "$(OUT)/$$f" ] || { echo "missing: $$f"; fail=1; }; \
	done; \
	if grep -RnE 'href="/|src="/' $(OUT)/*.html; then \
	  echo "root-absolute path found (breaks project Pages)"; fail=1; \
	fi; \
	[ $$fail -eq 0 ] && echo "check: ok"

clean:
	rm -rf $(OUT)
