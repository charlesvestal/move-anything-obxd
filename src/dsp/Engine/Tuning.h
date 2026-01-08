//
//  Tuning.h
//  OB-Xd
//
//  Stubbed for Move Anything port - MTS-ESP not supported
//

#pragma once

class Tuning
{
public:
    Tuning() {}
    ~Tuning() {}

    void updateMTSESPStatus() {
        // MTS-ESP not supported in this port
    }

    double tunedMidiNote(int midiIndex) {
        // Standard 12-TET tuning
        return (double)midiIndex;
    }

    bool hasMTSMaster() {
        return false;
    }

    const char *getMTSScale() {
        return "12-TET";
    }
};
