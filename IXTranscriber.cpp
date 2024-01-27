// IXTranscriber.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <chrono>
#include "Transcriber.h"

int main() {
    Transcriber transcriber(48'000); // Assuming 16000 is the sample rate

    while (1) {
        std::cout << "Press enter to start transcription" << std::endl;
        std::cin.get();

        auto start = std::chrono::high_resolution_clock::now();
        transcriber.start_transcription();
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        std::cout << "Start transcription took " << elapsed.count() << " seconds." << std::endl;

        std::cout << "Press enter to stop transcription" << std::endl;
        std::cin.get();

        start = std::chrono::high_resolution_clock::now();
        transcriber.stop_transcription();
        end = std::chrono::high_resolution_clock::now();
        elapsed = end - start;
        std::cout << "Stop transcription took " << elapsed.count() << " seconds." << std::endl;

        std::cout << "Enter q to quit, anything else to continue: ";
        std::string input;
        std::cin >> input;
        if (input == "q") {
            break;
        }
    }

    return 0;
}

// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
