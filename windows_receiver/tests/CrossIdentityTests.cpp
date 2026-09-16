#include "Cross/CrossIdentity.h"
#include <cassert>
#include <iostream>
int main() {
    using thunderlink::CrossIdentity;
    // Pure public-certificate helpers: these tests never create a user identity,
    // read credentials, or alter the Windows certificate/credential stores.
    const std::string client(64,'0'),server(64,'1');
    assert(CrossIdentity::IsFingerprint(client));
    assert(!CrossIdentity::IsFingerprint(std::string(63,'0')));
    assert(!CrossIdentity::IsFingerprint(std::string(64,'g')));
    assert(CrossIdentity::NormalizeFingerprint(std::string(64,'A'))==std::string(64,'a'));
    assert(CrossIdentity::VerificationCode(client,server)=="2C5A757AF655");
    assert(CrossIdentity::VerificationCode(server,client)!=CrossIdentity::VerificationCode(client,server));
    std::cout << "Cross identity fingerprint and shared pairing-code vector passed\n";
}
