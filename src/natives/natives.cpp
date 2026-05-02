#include "./natives.hpp"

#include "cli_catalog.hpp"

// `setupNatives` is now a thin compatibility shim around the catalog.
// `cli_catalog_install_all` produces the same set of globals in the
// same declaration order (plus a new `Zym` global registered as a
// regular catalog entry) and seeds the VM's capability set with every
// catalog name. See src/natives/cli_catalog.hpp for the rationale.
void setupNatives(ZymVM* vm)
{
    cli_catalog_install_all(vm);
}
