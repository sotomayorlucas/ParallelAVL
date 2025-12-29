# Makefile for Rigorous Parallel Trees IEEE Paper

TEX = pdflatex
BIBTEX = bibtex
PAPER = rigorous_parallel_trees

.PHONY: all paper clean view

all: paper

paper: $(PAPER).pdf

$(PAPER).pdf: $(PAPER).tex
	$(TEX) $(PAPER).tex
	$(TEX) $(PAPER).tex
	@echo ""
	@echo "════════════════════════════════════════"
	@echo "  Paper compiled successfully!"
	@echo "  Output: $(PAPER).pdf"
	@echo "════════════════════════════════════════"
	@echo ""

view: $(PAPER).pdf
	@if command -v xdg-open > /dev/null; then \
		xdg-open $(PAPER).pdf; \
	elif command -v open > /dev/null; then \
		open $(PAPER).pdf; \
	elif command -v start > /dev/null; then \
		start $(PAPER).pdf; \
	else \
		echo "Please open $(PAPER).pdf manually"; \
	fi

clean:
	rm -f $(PAPER).aux $(PAPER).log $(PAPER).out $(PAPER).bbl $(PAPER).blg
	@echo "Cleaned auxiliary files"

clean-all: clean
	rm -f $(PAPER).pdf
	@echo "Cleaned PDF"

help:
	@echo "Rigorous Parallel Trees Paper - IEEE Format"
	@echo ""
	@echo "Targets:"
	@echo "  make           - Compile paper to PDF"
	@echo "  make view      - Open PDF in viewer"
	@echo "  make clean     - Remove auxiliary files"
	@echo "  make clean-all - Remove PDF and auxiliary files"
	@echo ""
	@echo "Output: $(PAPER).pdf"
