#pragma once
#include <vector>
#include <cmath>

class SimpleReverb {
private:
    // 梳状滤波器：负责产生混响的“长尾巴”
    struct Comb {
        std::vector<float> buffer;
        size_t index = 0;
        float feedback;
        float damp = 0.7f;       // 高频衰减: 0=不衰减, 1=最大
        float lowDamp = 0.4f;    // 低频衰减: 0=不衰减, 1=最大
        float lpState = 0.0f;    // 单极低通状态
        float hpState = 0.0f;    // 单极高通状态
        float prevLp = 0.0f;     // 低通前一帧 (高通差分用)
        void resize(size_t size) { buffer.resize(size, 0); }
        float process(float input) {
            float output = buffer[index];
            // 单极低通: 衰减高频
            lpState = output * (1.0f - damp) + lpState * damp;
            // 单极高通: 衰减低频 (y[n] = a*(y[n-1] + x[n] - x[n-1]))
            float a_hp = 1.0f - lowDamp;
            float hpOut = a_hp * (hpState + lpState - prevLp);
            hpState = hpOut;
            prevLp = lpState;
            buffer[index] = input + hpOut * feedback;
            index = (index + 1) % buffer.size();
            return output;
        }
    };

    // 全通滤波器：负责涂抹回声，让声音不那么“颗粒感”
    struct Allpass {
        std::vector<float> buffer;
        size_t index = 0;
        void resize(size_t size) { buffer.resize(size, 0); }
        float process(float input) {
            float buffered = buffer[index];
            float output = -input + buffered;
            buffer[index] = input + (buffered * 0.5f);
            index = (index + 1) % buffer.size();
            return output;
        }
    };

    // 升级为 8 个梳状滤波器 (Freeverb 标准)
    std::vector<Comb> combs;
    // 升级为 4 个全通滤波器
    std::vector<Allpass> allpasses;

    float wetLevel = 0.5f;
    float roomSize = 0.84f;

public:
    void init(int sampleRate) {
        combs.resize(8);
        allpasses.resize(4);

        float scale = sampleRate / 44100.0f;

        // >>>>> 大厅级参数 (Schroeder/Freeverb 标准调教) >>>>>
        // 这些是互质数，防止共振频率重叠
        // 增加了延迟长度，模拟更大的空间
        combs[0].resize((size_t)(1116 * scale));
        combs[1].resize((size_t)(1188 * scale));
        combs[2].resize((size_t)(1277 * scale));
        combs[3].resize((size_t)(1356 * scale));
        combs[4].resize((size_t)(1422 * scale));
        combs[5].resize((size_t)(1491 * scale));
        combs[6].resize((size_t)(1557 * scale));
        combs[7].resize((size_t)(1617 * scale));

        allpasses[0].resize((size_t)(225 * scale));
        allpasses[1].resize((size_t)(341 * scale));
        allpasses[2].resize((size_t)(441 * scale));
        allpasses[3].resize((size_t)(556 * scale));

        setRoomSize(0.84f); // 默认大厅大小
    }

    void setRoomSize(float size) {
        roomSize = size;
        // 映射 0.0-1.0 到反馈量 0.7-0.95 (0.95 是非常长的拖尾)
        float feedback = 0.7f + (size * 0.28f);
        for(auto& c : combs) c.feedback = feedback;
    }

    void setMix(float mix) {
        wetLevel = mix;
    }

    void setDamp(float d) {
        for (auto& c : combs) c.damp = d;
    }

    void setLowDamp(float d) {
        for (auto& c : combs) c.lowDamp = d;
    }

    void process(float* buffer, int numFrames) {
        for (int i = 0; i < numFrames; ++i) {
            float inputL = buffer[i * 2];
            float inputR = buffer[i * 2 + 1];

            // 混合单声道输入，并应用增益 (0.3f)
            float input = (inputL + inputR) * 0.1f;

            float out = 0.0f;

            // 8个梳状滤波器并行处理
            for (auto& comb : combs) out += comb.process(input);

            // 4个全通滤波器串行处理
            for (auto& ap : allpasses) out = ap.process(out);

            // >>>>> 立体声扩展技巧 >>>>>
            // 左声道 = 干声 + 湿声
            buffer[i * 2]     = inputL + out * wetLevel;

            // 右声道 = 干声 - 湿声 (相位反转)
            // 这会产生极宽的立体声场感，听起来像在大厅
            buffer[i * 2 + 1] = inputR - out * wetLevel;
            // <<<<<<<<<<<<<<<<<<<<<<<<
        }
    }
};