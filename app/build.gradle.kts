plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.chenyinan.reface_cp_assist"
    compileSdk {
        version = release(36)
    }

    androidResources {
        noCompress.addAll(listOf("wav", "mp3", "ogg", "sf2"))
    }

    defaultConfig {
        applicationId = "com.chenyinan.reface_cp_assist"
        minSdk = 27
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"

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

    // >>>>> 1. 配置签名信息 (新增) >>>>>
    signingConfigs {
        create("release") {
            // 这里的路径取决于你刚才把 jks 文件存在哪了
            // 如果放在项目根目录，用 rootProject.file("my_keystore.jks")
            // 如果放在 app 目录下，用 file("my_keystore.jks")
            storeFile = file("C:\\Users\\easyh\\AndroidStudioProjects\\CynKeyApk\\cyn_keystore.jks")
            storePassword = "8931007" // 刚才设置的密码，例如 "123456"
            keyAlias = "key0"       // 刚才设置的别名
            keyPassword = "8931007" // 刚才设置的密码
        }
    }

    buildTypes {
        release {
            // >>>>> 2. 引用上面的签名配置 (新增) >>>>>
            signingConfig = signingConfigs.getByName("release")
            // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

            // 代码混淆 (通常 Release 默认开启)
            // 注意：如果开启混淆导致 JNI 找不到 Java 方法，可能需要关掉它
            // 为了测试性能，暂时建议先设为 false，排除混淆带来的干扰
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