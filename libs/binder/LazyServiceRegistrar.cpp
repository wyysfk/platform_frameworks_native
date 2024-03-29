/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AidlLazyServiceRegistrar"

#include <binder/LazyServiceRegistrar.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <android/os/BnClientCallback.h>
#include <android/os/IServiceManager.h>
#include <utils/Log.h>

namespace android {
namespace binder {
namespace internal {

using AidlServiceManager = android::os::IServiceManager;

class ClientCounterCallback : public ::android::os::BnClientCallback {
public:
    ClientCounterCallback() : mNumConnectedServices(0) {}

    bool registerService(const sp<IBinder>& service, const std::string& name,
                         bool allowIsolated, int dumpFlags);

protected:
    Status onClients(const sp<IBinder>& service, bool clients) override;

private:
    /**
     * Unregisters all services that we can. If we can't unregister all, re-register other
     * services.
     */
    void tryShutdown();

    /**
     * Counter of the number of services that currently have at least one client.
     */
    size_t mNumConnectedServices;

    struct Service {
        sp<IBinder> service;
        std::string name;
        bool allowIsolated;
        int dumpFlags;
    };
    /**
     * Number of services that have been registered.
     */
    std::vector<Service> mRegisteredServices;
};

bool ClientCounterCallback::registerService(const sp<IBinder>& service, const std::string& name,
                                            bool allowIsolated, int dumpFlags) {
    auto manager = interface_cast<AidlServiceManager>(
                    ProcessState::self()->getContextObject(nullptr));

    ALOGI("Registering service %s", name.c_str());

    if (!manager->addService(name.c_str(), service, allowIsolated, dumpFlags).isOk()) {
        ALOGE("Failed to register service %s", name.c_str());
        return false;
    }

    if (!manager->registerClientCallback(name, service, this).isOk())
    {
      ALOGE("Failed to add client callback for service %s", name.c_str());
      return false;
    }

    mRegisteredServices.push_back({service, name, allowIsolated, dumpFlags});

    return true;
}

/**
 * onClients is oneway, so no need to worry about multi-threading. Note that this means multiple
 * invocations could occur on different threads however.
 */
Status ClientCounterCallback::onClients(const sp<IBinder>& service, bool clients) {
    if (clients) {
        mNumConnectedServices++;
    } else {
        mNumConnectedServices--;
    }

    ALOGI("Process has %zu (of %zu available) client(s) in use after notification %s has clients: %d",
          mNumConnectedServices, mRegisteredServices.size(),
          String8(service->getInterfaceDescriptor()).string(), clients);

    if (mNumConnectedServices == 0) {
        tryShutdown();
    }

    return Status::ok();
}

void ClientCounterCallback::tryShutdown() {
    ALOGI("Trying to shut down the service. No clients in use for any service in process.");

    // This makes the same assumption as IServiceManager.cpp. Could dedupe if used in more places.
    auto manager = interface_cast<AidlServiceManager>(
            ProcessState::self()->getContextObject(nullptr));

    auto unRegisterIt = mRegisteredServices.begin();
    for (; unRegisterIt != mRegisteredServices.end(); ++unRegisterIt) {
        auto& entry = (*unRegisterIt);

        bool success = manager->tryUnregisterService(entry.name, entry.service).isOk();

        if (!success) {
            ALOGI("Failed to unregister service %s", entry.name.c_str());
            break;
        }
    }

    if (unRegisterIt == mRegisteredServices.end()) {
        ALOGI("Unregistered all clients and exiting");
        exit(EXIT_SUCCESS);
    }

    for (auto reRegisterIt = mRegisteredServices.begin(); reRegisterIt != unRegisterIt;
         reRegisterIt++) {
        auto& entry = (*reRegisterIt);

        // re-register entry
        if (!registerService(entry.service, entry.name, entry.allowIsolated, entry.dumpFlags)) {
            // Must restart. Otherwise, clients will never be able to get a hold of this service.
            ALOGE("Bad state: could not re-register services");
        }
    }
}

}  // namespace internal

LazyServiceRegistrar::LazyServiceRegistrar() {
    mClientCC = std::make_shared<internal::ClientCounterCallback>();
}

LazyServiceRegistrar& LazyServiceRegistrar::getInstance() {
    static auto registrarInstance = new LazyServiceRegistrar();
    return *registrarInstance;
}

status_t LazyServiceRegistrar::registerService(const sp<IBinder>& service, const std::string& name,
                                               bool allowIsolated, int dumpFlags) {
    if (!mClientCC->registerService(service, name, allowIsolated, dumpFlags)) {
        return UNKNOWN_ERROR;
    }
    return OK;
}

}  // namespace hardware
}  // namespace android