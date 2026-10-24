#pragma once

// Development upload service. Starting it neither changes the boot partition
// nor enables the separate server-provided firmware upgrade path.
class LocalOta {
public:
    static LocalOta& GetInstance();
    bool Start();

    LocalOta(const LocalOta&) = delete;
    LocalOta& operator=(const LocalOta&) = delete;

private:
    LocalOta() = default;
};
