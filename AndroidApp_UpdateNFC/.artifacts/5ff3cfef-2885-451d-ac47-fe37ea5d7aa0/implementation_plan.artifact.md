# Implementation Plan - Fix ApiClient.kt

The user wants to fix warnings and errors in `ApiClient.kt`.
Research revealed a critical runtime error in the Retrofit configuration and some security/code style warnings.

## Proposed Changes

### [Component] Network Layer

#### [MODIFY] [ApiClient.kt](file:///C:/Users/73181/OneDrive/Desktop/NCKH/SCA_THESIS/AndroidApp/app/src/main/java/com/example/uwb/network/ApiClient.kt)
- **Fix Critical Error**: Add a trailing slash to the `baseUrl`. Retrofit requires the base URL to end with a `/`, otherwise it throws an `IllegalArgumentException` at runtime.
- **Fix Security Warning**: Update `connectionSpecs` to include `ConnectionSpec.MODERN_TLS` alongside `ConnectionSpec.CLEARTEXT`. Restricting the client to only cleartext is considered insecure and may trigger lint warnings.
- **Improve Code Style**: Extract the base URL into a private constant `BASE_URL` for better readability and maintainability.

## Verification Plan

### Automated Tests
- Run `:app:assembleDebug` to ensure the project still builds successfully.
- Since this is a network configuration change, manual verification is also recommended.

### Manual Verification
- Deploy the app and verify that network requests to the local server (10.0.7.178:8000) are successful.
- Verify that no `IllegalArgumentException` is thrown during `ApiClient.api` initialization.
