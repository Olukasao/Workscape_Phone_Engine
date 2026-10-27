# Build de desenvolvimento no Linux (testa o protocolo e o SIP contra a central).
# O build de Windows usa o workflow em .github/workflows. Requer o PJSIP já compilado em third_party/.
PJDIR ?= ../third_party/pjproject-2.15.1
include $(PJDIR)/build.mak

CXXFLAGS_ENGINE = -std=c++17 -Wall -Wextra -O2 $(PJ_CXXFLAGS) $(APP_CXXFLAGS)
LDLIBS_ENGINE   = $(PJ_LDXXFLAGS) $(PJ_LDXXLIBS) $(APP_LDXXLIBS) -lpthread

bin/workscape-phone-engine: src/main.cpp third_party/json.hpp
	@mkdir -p bin
	$(PJ_CXX) $(CXXFLAGS_ENGINE) -o $@ src/main.cpp $(LDLIBS_ENGINE)

clean:
	rm -rf bin
