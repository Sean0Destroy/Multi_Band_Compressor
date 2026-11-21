#include <catch2/catch_session.hpp>
#include <juce_events/juce_events.h>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // MessageManager for Timer/APVTS; no display needed
    return Catch::Session().run (argc, argv);
}
