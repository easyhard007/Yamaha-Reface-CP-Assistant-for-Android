plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.example.cynarranger"
    compileSdk {
        version = release(36)
    }

    defaultConfig {
        applicationId = "com.example.cynarranger"
        minSdk = 27
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_shared"

                // >>>>> 新增：强制 16KB 对齐 >>>>>
                // -Wl 告诉编译器把后面的参数传给 Linker
                // -z,max-page-size=16384 设置最大页大小为 16KB
                cppFlags += "-Wl,-z,max-page-size=16384"

                // 强制开启 O3 优化 (即使在 Debug 模式)  -O3: 最高级别优化
                // -ffast-math: 允许编译器对浮点运算进行激进优化 (对音频处理很有用)
                cppFlags += "-O3 -ffast-math"
                // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildFeatures {
        viewBinding = true
        prefab = true // 开启 Prefab 功能（这是 Android 用来分发 C++ 库的机制）
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.constraintlayout)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    // 添加 Oboe
    implementation("com.google.oboe:oboe:1.9.3")
}