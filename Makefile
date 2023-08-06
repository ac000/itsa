TARGETS = itsa hdrchk

.PHONY: all $(TARGETS)
all: $(TARGETS)

MAKE_OPTS = --no-print-directory V=$V

.PHONY: itsa
itsa:
	@echo "Building: itsa"
	@$(MAKE) $(MAKE_OPTS) -C src/

.PHONY: hdrchk
hdrchk:
	@echo "Checking Headers"
	@$(MAKE) $(MAKE_OPTS) -C src/ hdrchk

.PHONY: clean
clean:
	@echo "Cleaning: itsa"
	@$(MAKE) $(MAKE_OPTS) -C src/ clean
