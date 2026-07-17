#include <spdk/nvme.h>

int main() {
    enum spdk_nvme_transport_type transport = SPDK_NVME_TRANSPORT_PCIE;
    return spdk_nvme_transport_id_parse_trtype(&transport, "pcie") == 0 ? 0 : 1;
}
